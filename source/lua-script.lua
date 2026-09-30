local uBit = require("microbit")
require("microbit.audio")
require("microbit.display")
local radio = require("microbit.radio")
local serial = require("microbit.serial")
-- at the prompt as before require came, for the lessons that use them
require("microbit.accelerometer")
require("microbit.compass")
require("microbit.io")
require("microbit.i2c")
require("tpbot")

-- What the REPL cannot do without, taken here so that the prompt comes
-- back even after a line takes away the global, and answers the line
-- that puts it back: a line is compiled by loadstring, given its
-- environment by setfenv and run under pcall; what it says goes out
-- through gsub and sub, and a line is put together with concat.
local pcall = pcall
local loadstring, setfenv = loadstring, setfenv
local gsub, sub, concat = string.gsub, string.sub, table.concat
-- and the port's own: arming it for its next event, and marking the
-- command read from it as the REPL's
local arm_port, event_repl = serial.eventAfterAsync, uBit.eventRepl
local HEAD_MATCH = uBit.CODAL_SERIAL_EVT_HEAD_MATCH
local CLICK = uBit.DEVICE_BUTTON_EVT_CLICK
local LONG_CLICK = uBit.DEVICE_BUTTON_EVT_LONG_CLICK

local heart = {
  width = 10,
  height = 5,
  data = {
      0,   0,   0,   0,   0,    0, 255,   0, 255,   0,
      0, 255,   0, 255,   0,  255,  64, 255,  64, 255,
      0, 255, 255, 255,   0,  255,  64,  64,  64, 255,
      0,   0, 255,   0,   0,    0, 255,  64, 255,   0,
      0,   0,   0,   0,   0,    0,   0, 255,   0,   0
  }
}

uBit.audio.setVolume(20)
uBit.audio.express("giggle")
uBit.display.animate(heart, 1000, 5)
uBit.display.scrollAsync(uBit.friendlyName())

-- The session being served, if any
local active_session

-- Output goes wherever the session being served takes it.
-- The serial session names no other way out, so it falls to
-- the port.
local function write(s)
  local session = active_session
  local out = session and session.transport.send
  if out then return out(s) end
  -- one send for each 64 characters, each a copy on CODAL's heap, "\n"
  -- made "\r\n" in the piece: a copy of the whole of s could need as
  -- much heap again as s itself
  for i = 1, #s, 64 do
    serial.send((gsub(sub(s, i, i + 63), "\n", "\r\n")))
  end
end

io = {
  write = write
}

local function is_identifier(str)
  return type(str) == "string"
    and string.match(str, "^[%a_][%w_]*$")
end

local prettyprint = { }

local function serialize(value, visited)
  visited = visited or {}
  local pp = prettyprint[type(value)]
  if pp then
    return pp(value, visited)
  end
  return tostring(value)
end

function prettyprint.string(value)
  return string.format("%q", value)
end

local function key(k, visited)
  if is_identifier(k) then
    return k
  else
    return "[" .. serialize(k, visited) .. "]"
  end
end

local function table_tokens(value, visited)
  local out = {"{"}
  local first = true
  for k, v in pairs(value) do
    if not first then
       table.insert(out, ", ")
    end
    first = false
    table.insert(out, key(k, visited) .. " = " .. serialize(v, visited))
  end
  table.insert(out, "}")
  return out
end

function prettyprint.table(value, visited)
  if visited[value] then
    return "<cycle>"
  end
  visited[value] = true
  local out = table_tokens(value, visited)
  visited[value] = nil
  return table.concat(out)
end

local function print_values(serialize, ...)
  local n = select("#", ...)
  if n == 0 then
    return
  end
  local out = { }
  for i = 1, n do
    table.insert(out, serialize(select(i, ...)))
  end
  write(table.concat(out, "\t") .. "\n")
end

local env = { }
setmetatable(env, {
  __index = _G
})

local function load_with_env(code, chunkname)
  local fn, err = loadstring(code, chunkname)
  if fn then
    setfenv(fn, env)
  end
  return fn, err
end

local function is_incomplete(err)
  return err and string.find(err, "near '<eof>'", 1, true) ~= nil
end

local function compile_try(code)
  local chunk, err = load_with_env(code, "REPL")
  if chunk then
    return chunk
  elseif is_incomplete(err) then
    return nil, err, true
  end
  return chunk, err, false
end

local function compile(code)
  local chunk, err, incomp = compile_try(code)
  if incomp then
    local e_chunk, e_err = compile_try("return " .. code)
    if e_chunk then
      return e_chunk, e_err
    end
    return chunk, err, true
  end
  if chunk then
    local e_chunk, e_err = compile_try("return " .. code)
    if e_chunk then
      return e_chunk, e_err
    end
    return chunk
  end
  local e_chunk, e_err, e_incomp =
    compile_try("return " .. code)
  if e_chunk or e_incomp then
    return e_chunk, e_err, e_incomp
  end
  return nil, err or e_err, false
end

-- The REPL's own output. print is the same, for programs, and a
-- program may put its own in its place.
local function say(...)
  print_values(tostring, ...)
end

print = say

collectgarbage("setpause", 100)
-- "unknown" where the firmware gives no version
local version = uBit.version and uBit.version() or "unknown"
say("micro:bit\nLua 5.1 REPL\nfirmware " .. version)

local function execute(chunk)
  local results = { pcall(chunk) }
  if results[1] then
    if #results > 1 then
      local out = { }
      for i = 2, #results do
        table.insert(out, serialize(results[i]))
      end
      say("=> " .. table.concat(out, "\t"))
    end
  else
    say("Runtime error: " .. tostring(results[2]))
  end
end

-- A mistake on the way from a line to its result, said
local function say_error(err)
  say("Runtime error: " .. tostring(err))
end

-- A REPL session: a buffer plus the compile-driven submit loop. The
-- same engine is used for the serial console and the BLE UART service.
-- Each function stands at most one level inside another: the parser's
-- frame for a function nested deeper costs over half a kilobyte of stack
-- while the script is read at boot.
local function make_session(transport)
  local s = {
    transport = transport,
    buffer = "",
  }
  function s.prompt()
    write(s.buffer == "" and "> " or ">> ")
  end
  function s.submit(text)
    s.buffer = s.buffer .. text .. "\n"
    if s.transport.crlf_before_result then
      write("\r\n")
    end
    local chunk, err, incomplete = compile(s.buffer)
    if not incomplete then
      if chunk then
        execute(chunk)
      else
        say("Compile error: " .. tostring(err))
      end
      s.buffer = ""
    end
    pcall(collectgarbage, "collect")
    s.prompt()
  end
  -- f, with what it says going to this session. Whatever goes wrong in
  -- it, even showing a result that cannot be shown, or running out of
  -- memory where Lua's allocator can refuse (on the board CODAL stops
  -- with 020 first), the line is over, the mistake is said if it can be,
  -- and the prompt comes back. One pcall here covers the reading of the port,
  -- the line and its result: each pcall more on the way to a command
  -- would cost the C stack about 440 bytes.
  function s.run(f, ...)
    local saved = active_session
    active_session = s
    local ran, err = pcall(f, ...)
    if not ran then
      s.buffer = ""
      pcall(collectgarbage, "collect")
      if not pcall(say_error, err) then
        pcall(say, "Runtime error: the result cannot be shown")
      end
      pcall(collectgarbage, "collect")
      pcall(s.prompt)
    end
    active_session = saved
  end
  return s
end

local serial_session = make_session({
  crlf_before_result = true,
  getChar = serial.getCharAsync,
  arm = function() arm_port(1) end
})

local handler = { }

microbit.handler = handler

local function enter()
  serial_session.submit("")
end

local function backspace()
  if #serial_session.buffer > 0 then
    serial_session.buffer = string.sub(serial_session.buffer, 1, -2)
    write("\b \b")
  end
end

local keypress = {
  ["\r"] = enter,
  ["\n"] = enter,
  ["\b"] = backspace,
  ["\127"] = backspace
}

local typed_here = ""

-- The characters typed since the last key the REPL acts on, held to be
-- added to the line, and echoed, in one piece: 64 at most, and a table
-- that held more than 16 is let go, so that a paste leaves no table of
-- its size behind
local held, held_count = { }, 0

local function flush_held()
  if held_count > 0 then
    local text = concat(held, "", 1, held_count)
    if held_count > 16 then
      held = { }
    end
    held_count = 0
    serial_session.buffer = serial_session.buffer .. text
    write(text)
  end
end

-- What has been typed, from c on, into the console
local function read_port(c)
  -- what a read cut short by a mistake held goes, as the line does
  held_count = 0
  c = c or serial_session.transport.getChar()
  while c do
    local input = keypress[c]
    if input then
      flush_held()
      input()
    else
      held_count = held_count + 1
      held[held_count] = c
      if held_count == 64 then
        flush_held()
      end
    end
    c = serial_session.transport.getChar()
  end
  flush_held()
end

-- The port is armed however the reading ends, and then looked at once
-- more: a character that came before it was armed raises no event.
local function port_to_console(value)
  if value == HEAD_MATCH then
    -- a command's sleeps handle events, whichever on_event passed the
    -- port's event on to here
    event_repl()
    local c
    repeat
      serial_session.run(read_port, c)
      serial_session.transport.arm()
      c = serial_session.transport.getChar()
    until not c
  end
end

handler[microbit.DEVICE_ID_SERIAL] = port_to_console

local function button(value, btn)
  if value == CLICK then
      uBit.display.scroll(btn)
   elseif value == LONG_CLICK then
      uBit.display.scroll(btn .. "!")
  end
end

handler[microbit.DEVICE_ID_BUTTON_A] = function(value)
  button(value, "A")
end

handler[microbit.DEVICE_ID_BUTTON_B] = function(value)
  button(value, "B")
end

handler[microbit.DEVICE_ID_BUTTON_AB] = function(value)
  button(value, "AB")
end

local function dispatch(source, value, timestamp)
  local handle = handler[source]
  if handle then
    handle(value, timestamp)
  end
end

-- A program may put its own on_event in place of this one. When
-- on_event is not a function, events come here all the same.
on_event = dispatch
uBit.eventFallback(dispatch)


-- A REPL over a radio link, and the other end of it.
--
-- Both turn the radio on themselves; the group is the one
-- every micro:bit starts in. A board that calls again — one
-- that was reset, or lost the link — is taken as it comes,
-- and the session starts over for it.
--
-- listen(name) waits for that board to call, then serves it:
-- what arrives over the link is typed into a session of its
-- own, and what the session says goes back the same way.
--
-- connect(name, timeout) calls, then carries the port over:
-- what is typed here goes out, what comes back is printed.

-- made when listen() is first called: most boards never serve a link
local radio_session


-- A piece of the link, as much or as little as arrived: what
-- stands before a line ending is entered, what follows it
-- waits for the rest to come.
local function typed(piece)
  local at = string.find(piece, "[\r\n]")
  while at do
    radio_session.buffer =
      radio_session.buffer .. string.sub(piece, 1, at - 1)
    radio_session.submit("")
    piece = string.sub(piece, at + 1)
    at = string.find(piece, "[\r\n]")
  end
  radio_session.buffer = radio_session.buffer .. piece
end

--- A fresh session for a caller that has just arrived
local function greet()
  radio_session.buffer = ""
  radio_session.run(radio_session.prompt)
end

function listen(name)
  radio_session = radio_session or make_session({
    crlf_before_result = false,
    send = function(text) radio.tx(text) end
  })
  radio.enable()
  radio.listen(name)
  greet()
  while true do
    if radio.answered(name) then greet() end
    local piece, starts = radio.rx()
    if piece then
      -- each message is whole lines: one that starts drops what follows
      -- the last line ending, a line whose end was lost on the way, which
      -- the other board said; the lines of a statement still open stay
      if starts then
        radio_session.buffer = string.match(radio_session.buffer, "^.*\n")
          or ""
      end
      radio_session.run(typed, piece)
    end
    uBit.sleep(5)
  end
end

-- Whatever has been typed since the last look
local function typing()
  local chars = { }
  local c = serial.getCharAsync()
  while c do
    chars[#chars + 1] = c
    c = serial.getCharAsync()
  end
  return table.concat(chars)
end

--- What the link says goes to the port, all that has come
local function link_to_port()
  local piece = radio.rx()
  while piece do
    write(piece)
    piece = radio.rx()
  end
end

-- Whoever has the port serves it: the console's own session
-- to start with, the link once connect() has opened one.
-- connect puts the other one in place; nothing asks which.
-- A line at a time goes over the link: tx waits to be
-- answered, and a character each would spend that wait while
-- the next ones pile up in the port. So the typing is echoed
-- as it comes and held until its line is whole.
local function port_to_link(value)
  if value == HEAD_MATCH then
    local text = typing()
    arm_port(1)
    write(text)
    typed_here = typed_here .. text
    local at = string.find(typed_here, "[\r\n]")
    while at do
      local line = string.sub(typed_here, 1, at)
      typed_here = string.sub(typed_here, at + 1)
      if not radio.tx(line) then
        -- tx has tried for a quarter of a second; what was typed after
        -- the line would arrive without it, and goes too. The other board
        -- may have the line all the same, when only its answers were lost.
        local after = string.find(typed_here, "[^\r\n]")
        typed_here = ""
        write("\nThe other micro:bit did not answer, so it may not have got: "
              .. string.sub(line, 1, -2) .. "\n"
              .. (after and "What you typed after it was not sent either.\n"
                  or "")
              .. "It may still be running a command. Once it has finished,"
              .. " check whether the line ran before you type it again.\n")
        return
      end
      -- what the other board said meanwhile, before its inbox here fills
      link_to_port()
      at = string.find(typed_here, "[\r\n]")
    end
  end
end

local DEVICE_ID_SERIAL = uBit.DEVICE_ID_SERIAL
local DEVICE_ID_RADIO = uBit.DEVICE_ID_RADIO

function connect(name, timeout)
  radio.enable()
  if not radio.connect(name, timeout) then
    say("Connection timed out.")
    return
  end
  say(name .. " connected.")
  typed_here = ""
  handler[DEVICE_ID_SERIAL] = port_to_link
  handler[DEVICE_ID_RADIO] = link_to_port
end

-- Script-level setup (runs once before the main fiber
-- is released):
-- show the prompt, then take what was typed while the
-- script ran, which also arms the port's per-char
-- head-match event. After this returns, release_fiber() in
-- main() hands control to the scheduler; on_event() handles
-- all events from the bus.
serial_session.prompt()
port_to_console(HEAD_MATCH)
