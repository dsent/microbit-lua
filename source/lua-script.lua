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
-- and what tells an unfinished line and shows a result: find, match and
-- format
local find, match, format = string.find, string.match, string.format
-- and the port's own: arming it for its next event, and marking the
-- command read from it as the REPL's
local arm_port, event_repl = serial.eventAfterAsync, uBit.eventRepl
-- The radio link's session is the same way; the link's own tx and rx
-- are taken when a link opens.
local get_char = serial.getCharAsync
local HEAD_MATCH = uBit.CODAL_SERIAL_EVT_HEAD_MATCH
local DEVICE_ID_SERIAL = uBit.DEVICE_ID_SERIAL
local DEVICE_ID_RADIO = uBit.DEVICE_ID_RADIO
local CLICK = uBit.DEVICE_BUTTON_EVT_CLICK
local LONG_CLICK = uBit.DEVICE_BUTTON_EVT_LONG_CLICK

local heart = {
  width = 10,
  height = 5,
  data = string.char(
      0,   0,   0,   0,   0,    0, 255,   0, 255,   0,
      0, 255,   0, 255,   0,  255,  64, 255,  64, 255,
      0, 255, 255, 255,   0,  255,  64,  64,  64, 255,
      0,   0, 255,   0,   0,    0, 255,  64, 255,   0,
      0,   0,   0,   0,   0,    0,   0, 255,   0,   0
  )
}

uBit.audio.setVolume(20)
uBit.audio.express("giggle")
uBit.display.animate(heart, 1000, 5)
uBit.display.scrollAsync(uBit.friendlyName())

-- The session being served, if any
local active_session

-- Output goes wherever the session being served takes it: the link's
-- session sends it over the link, any other to the port. Either way it
-- goes in pieces of 64 characters, each a copy on CODAL's heap, "\n" made
-- "\r\n" in the piece: a copy of the whole of s could need as much heap
-- again as s itself, and the link carries what the port would show.
local function write(s)
  local session = active_session
  local out = session and session.transport.send or serial.send
  for i = 1, #s, 64 do
    out((gsub(sub(s, i, i + 63), "\n", "\r\n")))
  end
end

io = {
  write = write
}

local function is_identifier(str)
  return type(str) == "string"
    and match(str, "^[%a_][%w_]*$")
end

local serialize

local function key(k, visited)
  if is_identifier(k) then
    return k
  end
  return "[" .. serialize(k, visited) .. "]"
end

-- A value as a line of Lua: a string quoted, a table with its
-- keys and values, a table inside itself as <cycle>
function serialize(value, visited)
  local t = type(value)
  if t == "string" then
    return format("%q", value)
  elseif t ~= "table" then
    return tostring(value)
  end
  visited = visited or { }
  if visited[value] then
    return "<cycle>"
  end
  visited[value] = true
  local out, n = { "{" }, 1
  for k, v in pairs(value) do
    n = n + 1
    out[n] = (n > 2 and ", " or "") .. key(k, visited) .. " = "
      .. serialize(v, visited)
  end
  out[n + 1] = "}"
  visited[value] = nil
  return concat(out)
end

local function print_values(serialize, ...)
  local n = select("#", ...)
  if n == 0 then
    return
  end
  local out = { }
  for i = 1, n do
    out[i] = serialize(select(i, ...))
  end
  write(concat(out, "\t") .. "\n")
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
  return err and find(err, "near '<eof>'", 1, true) ~= nil
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
  local e_chunk, e_err, e_incomp = compile_try("return " .. code)
  if e_chunk then
    return e_chunk
  end
  local chunk, err, incomp = compile_try(code)
  if chunk or incomp then
    return chunk, err, incomp
  end
  if e_incomp then
    return nil, e_err, true
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
        out[i - 1] = serialize(results[i])
      end
      say("=> " .. concat(out, "\t"))
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
-- same engine is used for the serial console and the radio link.
-- Each function stands at most one level inside another: the parser's
-- frame for a function nested deeper costs over half a kilobyte of stack
-- while the script is read at boot.
local function make_session(transport)
  local s = {
    transport = transport,
    buffer = "",
  }
  function s.prompt()
    if not s.transport.taken then
      write(s.buffer == "" and "> " or ">> ")
    end
  end
  function s.submit(text)
    s.buffer = s.buffer .. text .. "\n"
    write("\n")
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
  -- Keys as they come over the link, as the port's come to the console:
  -- a run of plain characters goes into the line and is echoed, Backspace
  -- takes one back, Enter ends the line, and Ctrl+C drops the line begun
  -- and starts afresh.
  function s.keys(text)
    local at = find(text, "\3[^\3]*$")
    if at then
      s.buffer = ""
      write("\n")
      s.prompt()
      text = sub(text, at + 1)
    end
    at = find(text, "[\r\n\b\127]")
    while at do
      local plain, key = sub(text, 1, at - 1), sub(text, at, at)
      if #plain > 0 then
        s.buffer = s.buffer .. plain
        write(plain)
      end
      if key == "\r" or key == "\n" then
        s.submit("")
      elseif #s.buffer > 0 then
        s.buffer = sub(s.buffer, 1, -2)
        write("\b \b")
      end
      text = sub(text, at + 1)
      at = find(text, "[\r\n\b\127]")
    end
    if #text > 0 then
      s.buffer = s.buffer .. text
      write(text)
    end
  end
  return s
end

local serial_session = make_session({
  getChar = serial.getCharAsync,
  arm = function() arm_port(1) end
})

local handler = { }

uBit.handler = handler

local function enter()
  serial_session.submit("")
end

local function backspace()
  if #serial_session.buffer > 0 then
    serial_session.buffer = sub(serial_session.buffer, 1, -2)
    write("\b \b")
  end
end

-- Ctrl+C: the line begun is dropped, and a new one starts
local function afresh()
  serial_session.buffer = ""
  write("\n")
  serial_session.prompt()
end

local keypress = {
  ["\r"] = enter,
  ["\n"] = enter,
  ["\b"] = backspace,
  ["\127"] = backspace,
  ["\3"] = afresh
}

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

-- What read_port was given and had not read when connect() took the port
local left_over = ""

-- What has been typed into the console: the characters given first, then
-- the port's
local function read_port(pending)
  -- what a read cut short by a mistake held goes, as the line does
  held_count = 0
  local at, n = 0, pending and #pending or 0
  while true do
    local c
    at = at + 1
    if at <= n then
      c = sub(pending, at, at)
    else
      c = serial_session.transport.getChar()
    end
    if not c then
      break
    end
    local input = keypress[c]
    if input then
      flush_held()
      input()
      -- connect() gave the port to the link: what follows is the link's
      if serial_session.transport.taken then
        left_over = at < n and sub(pending, at + 1) or ""
        return
      end
    else
      held_count = held_count + 1
      held[held_count] = c
      if held_count == 64 then
        flush_held()
      end
    end
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
      if serial_session.transport.taken then
        return handler[DEVICE_ID_SERIAL](value)
      end
      serial_session.transport.arm()
      c = serial_session.transport.getChar()
    until not c
  end
end

handler[DEVICE_ID_SERIAL] = port_to_console

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
-- Both turn the radio on themselves; the group is the one every micro:bit
-- starts in. A board that calls again, one that was reset or lost the
-- link, is taken as it comes, and the session starts over for it.
--
-- listen(name) waits for that board to call, or for any board if no name
-- is given, and from then on serves only the one that called: the keys
-- that arrive over the link are typed into a session of its own, as the
-- port's keys are into the console, and what the session says goes back
-- the same way. All the while it sends beacons, which say who it is and
-- whom it waits for; microbit.radio.scan() elsewhere lists them. listen()
-- typed over the link changes whom it waits for, and lets go of the board
-- that typed it. Ctrl+C or Ctrl+D on this board's own port ends it: the
-- link is closed, and both boards are back at their consoles.
--
-- connect(name, timeout) calls, then carries the port over: what is typed
-- here goes out as it is, the other board echoes and edits it, and what
-- comes back goes on the port as it is, so the port shows what that
-- board's own would. Ctrl+D hands the port back to the console; the other
-- board keeps listening, so the link can be made again. A link the other
-- board has closed, or has sent no beacon over for a while, is over too,
-- and the console says so.

-- made when listen() is first called: most boards never serve a link
local radio_session

-- the link's, taken when a link opens
local tx, rx

--- A fresh session for a caller that has just arrived
local function greet()
  radio_session.buffer = ""
  radio_session.run(radio_session.prompt)
end

-- Whatever has been typed since the last look: nothing, mostly, and
-- then no table is made for it
local function typing()
  local c = get_char()
  if not c then
    return ""
  end
  local chars, n = { c }, 1
  c = get_char()
  while c do
    n = n + 1
    chars[n] = c
    c = get_char()
  end
  return concat(chars)
end

local listening = false
local awaited   -- the board listen() waits for; nil for any

function listen(name)
  if name ~= nil and #name ~= 5 then
    error("a board name is five letters", 0)
  end
  if serial_session.transport.taken then
    error("this micro:bit is connected to another: press Ctrl+D first", 0)
  end
  awaited = name
  if listening then
    -- typed over the link: the caller is let go, and the loop below goes
    -- on waiting for the board now named
    radio.close()
    return
  end
  local answered, pause = radio.answered, uBit.sleep
  tx, rx = radio.tx, radio.rx
  radio_session = radio_session or make_session({ send = tx })
  listening = true
  radio.enable()
  -- The port is read here: the console's handler arms it for its next
  -- event only once the line that called listen() is over. Ctrl+C or
  -- Ctrl+D ends the listening; the rest of what is typed is dropped.
  while not find(typing(), "[\3\4]") do
    radio.beacon(awaited)
    local caller = answered(awaited)
    if caller then
      awaited = caller
      greet()
    end
    local piece = rx()
    if piece then
      radio_session.run(radio_session.keys, piece)
    end
    pause(5)
  end
  listening = false
  radio.close()
end

local peer       -- the board connect() reached
local mid_line   -- what it sent last left a line open on the port

--- The link is closed, and the port goes back to the console
local function disconnect()
  radio.close()
  handler[DEVICE_ID_SERIAL] = port_to_console
  handler[DEVICE_ID_RADIO] = nil
  serial_session.transport.taken = false
  write((mid_line and "\n" or "") .. peer .. " disconnected.\n")
  serial_session.prompt()
end

--- What the link says goes to the port as it comes, all that has come;
--- false is the other board closing the link, or its silence
local function link_to_port()
  local piece = rx()
  while piece do
    serial.send(piece)
    mid_line = sub(piece, -1) ~= "\n"
    piece = rx()
  end
  if piece == false then
    disconnect()
  end
end

-- What is typed goes over the link as it is, and the other board echoes
-- and edits it. tx waits to be answered; what is typed meanwhile waits in
-- the port and goes with the next piece, and a piece the other board does
-- not take, busy with a command of its own, is lost. What comes before a
-- Ctrl+D still goes over; what follows it is the console's. The port is
-- armed before it is read: a character that comes after the read raises
-- the event, and one before it is read now.
local function port_to_link(value)
  if value ~= HEAD_MATCH then
    return
  end
  arm_port(1)
  local text = typing()
  while true do
    local before, after = match(text, "^([^\4]*)\4(.*)")
    if not before then
      if #text > 0 then
        tx(text)
      end
      return
    end
    if #before > 0 then
      tx(before)
    end
    disconnect()
    left_over = ""
    serial_session.run(read_port, after)
    if not serial_session.transport.taken then
      return
    end
    -- connect() came after the Ctrl+D: what followed it is the new link's
    text = left_over
  end
end

function connect(name, timeout)
  if listening then
    error("this micro:bit is listening: press Ctrl+C on its own port first", 0)
  end
  if serial_session.transport.taken then
    error("this micro:bit is connected already: press Ctrl+D first", 0)
  end
  radio.enable()
  if not radio.connect(name, timeout) then
    say("Connection timed out.")
    return
  end
  say(name .. " connected.")
  tx, rx = radio.tx, radio.rx
  peer, mid_line = name, false
  handler[DEVICE_ID_SERIAL] = port_to_link
  handler[DEVICE_ID_RADIO] = link_to_port
  -- what is typed from here on goes over the link, what waits in the
  -- port included
  serial_session.transport.taken = true
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
