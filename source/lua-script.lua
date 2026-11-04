local uBit = require("microbit")
require("microbit.accelerometer")
require("microbit.io")
local audio = require("microbit.audio")
local display = require("microbit.display")
local radio = require("microbit.radio")
local serial = require("microbit.serial")

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

audio.express("giggle")
display.animate(heart, 1000, 5)
display.scrollAsync(uBit.friendlyName())

-- Output goes wherever the session being served takes it, a
-- line ending in CR LF as on the port. The serial session
-- names no other way out, so it falls to the port.
local function write(s)
  local session = active_session
  local out = session and session.send or serial.send
  out((string.gsub(s, "\n", "\r\n")))
end

io = {
  write = write
}

local unpack = unpack or table.unpack

local function is_identifier(str)
  return type(str) == "string"
    and str:match("^[%a_][%w_]*$")
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
  return err and err:find("near '<eof>'", 1, true) ~= nil
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

function print(...)
  print_values(tostring, ...)  
end

collectgarbage("setpause", 100)
-- "unknown" where the firmware gives no version
local version = uBit.version and uBit.version() or "unknown"
print("micro:bit\nLua 5.1 REPL\nfirmware " .. version)

local function execute(chunk)
  local results = { pcall(chunk) }
  if results[1] then
    if #results > 1 then
      local out = { }
      for i = 2, #results do
        table.insert(out, serialize(results[i]))
      end
      print("=> " .. table.concat(out, "\t"))
    end
  else
    print("Runtime error: " .. tostring(results[2]))
  end
end

-- A REPL session: a buffer plus the compile-driven submit loop.
-- The serial console and the radio link each have one; send is
-- where its output goes, the port when there is none.
local function make_session(send)
  local s = {
    send = send,
    buffer = "",
  }
  function s.prompt()
    write(s.buffer == "" and "> " or ">> ")
  end
  function s.submit()
    s.buffer = s.buffer .. "\n"
    write("\n")
    local chunk, err, incomplete = compile(s.buffer)
    if not incomplete then
      if chunk then
        execute(chunk)
      else
        print("Compile error: " .. tostring(err))
      end
      s.buffer = ""
    end
    collectgarbage("collect")
    s.prompt()
  end
  function s.backspace()
    if #s.buffer > 0 then
      s.buffer = s.buffer:sub(1, -2)
      write("\b \b")
    end
  end
  --- What was typed: a run of plain characters goes into the
  --- line and is echoed; the key after it erases or enters.
  --- Ctrl+C drops all that came before it and starts afresh.
  function s.keys(text)
    local rest = text:match(".*\3(.*)")
    if rest then
      s.buffer = ""
      write("\n")
      s.prompt()
      text = rest
    end
    for plain, key in string.gmatch(text, "([^\r\n\b\127]*)(.?)") do
      if #plain > 0 then
        s.buffer = s.buffer .. plain
        write(plain)
      end
      if key == "\b" or key == "\127" then
        s.backspace()
      elseif #key > 0 then
        s.submit()
      end
    end
  end
  function s.run(f)
    local saved = active_session
    active_session = s
    f()
    active_session = saved
  end
  return s
end

local serial_session = make_session()

local handler = { }

uBit.handler = handler

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

local function to_console(text)
  serial_session.run(function() serial_session.keys(text) end)
end

-- Where what is typed goes: the console's own session, or the
-- link while connect() has one open. The port is watched again
-- only once it is empty: a piece is dealt with before the next
-- one is taken. Then it is looked at once more: a character
-- that came between the last look and the arming raises no
-- event, and would wait there for the next key.
local typed_to = to_console

handler[uBit.DEVICE_ID_SERIAL] = function(value)
  if value == uBit.CODAL_SERIAL_EVT_HEAD_MATCH then
    local text = typing()
    repeat
      while #text > 0 do
        typed_to(text)
        text = typing()
      end
      serial.eventAfterAsync(1)
      text = typing()
    until #text == 0
  end
end

local smiley = {
  height = 5,
  width = 5,
  data = string.char(
      0, 255,   0, 255,   0,
      0, 255,   0, 255,   0,
      0,   0,   0,   0,   0,
    255,   0,   0,   0, 255,
      0, 255, 255, 255,   0
  )
}

local wink_A = {
  height = 2,
  width = 5,
  data = string.char(
      0,   0,   0, 255,   0,
    255, 255,   0, 255,   0
  )
}

local wink_B = {
  height = 2,
  width = 5,
  data = string.char(
      0, 255,   0,   0,   0,
      0, 255,   0, 255, 255
  )
}

local squint = {
  height = 2,
  width = 5,
  data = string.char(
      0,   0,   0,   0,   0,
    255, 255,   0, 255, 255
  )
}

local function button(value, wink)
  if value == uBit.DEVICE_BUTTON_EVT_DOWN then
    display.show(wink)
  elseif value == uBit.DEVICE_BUTTON_EVT_UP then
    display.show(smiley)
  end
end

gesture = {
  [uBit.ACCELEROMETER_EVT_TILT_UP] = display.rotateUp,
  [uBit.ACCELEROMETER_EVT_FACE_UP] = display.rotateUp,
  [uBit.ACCELEROMETER_EVT_TILT_DOWN] = display.rotateDown,
  [uBit.ACCELEROMETER_EVT_TILT_LEFT] = display.rotateLeft,
  [uBit.ACCELEROMETER_EVT_TILT_RIGHT] = display.rotateRight,
  [uBit.ACCELEROMETER_EVT_SHAKE] = function()
    audio.express("hello")
  end
}

handler[uBit.DEVICE_ID_BUTTON_A] = function(value)
  button(value, wink_A)
end

handler[uBit.DEVICE_ID_BUTTON_B] = function(value)
  button(value, wink_B)
end

handler[uBit.MICROBIT_ID_LOGO] = function(value)
  button(value, squint)
end

handler[uBit.DEVICE_ID_DISPLAY] = function(value)
  if value == uBit.DISPLAY_EVT_ANIMATION_COMPLETE then
    display.show(smiley)
  end
end

handler[uBit.DEVICE_ID_GESTURE] = function(value)
  local sensed = gesture[value]
  if sensed then
    sensed()
  end
end

function on_event(source, value, timestamp)
  local handle = handler[source]
  if handle then
    handle(value, timestamp)
  end
end

-- Gesture sensing requires accelerometer initialization
uBit.accelerometer.getSample()

-- A REPL over a radio link, and the other end of it.
--
-- Both turn the radio on themselves; the group is the one
-- every micro:bit starts in. A board that calls again — one
-- that was reset, or lost the link — is taken as it comes,
-- and the session starts over for it.
--
-- listen(name) waits for that board to call, or for any board
-- if no name is given, and from then on serves only the one
-- that called: what arrives over the link is typed into a
-- session of its own, and what the session says goes back the
-- same way. All the while it sends beacons, which say who it
-- is and whom it waits for; scan() elsewhere lists them.
-- listen() typed over the link changes whom it waits for, and
-- lets go of the board that typed it.
-- Ctrl+C or Ctrl+D on this board's own port ends it: the link
-- is closed, and both boards are back at their consoles.
--
-- connect(name, timeout) calls, then carries the port over:
-- what is typed here goes out, what comes back goes on the
-- port as it is, and the console here says no more. Ctrl+D
-- hands the port back to the console; the far end keeps
-- listening, so the link can be made again. A link whose far
-- end has gone quiet, with no beacon for a while, is over too.

local radio_session = make_session(radio.tx)


--- A fresh session for a caller that has just arrived
local function greet()
  radio_session.buffer = ""
  radio_session.run(radio_session.prompt)
end

local listening = false
local awaited   -- the board listen() waits for; nil for any

function listen(name)
  -- said before listening starts: a mistyped name would leave it on, and
  -- the next listen() would only stop it
  if name ~= nil and (type(name) ~= "string" or #name ~= 5) then
    error("a board name is five letters", 0)
  end
  awaited = name
  if listening then
    radio.close()
    return
  end
  listening = true
  radio.enable()
  while not typing():find("[\3\4]") do
    radio.beacon(awaited)
    local caller = radio.answered(awaited)
    if caller then
      awaited = caller
      greet()
    end
    local piece = radio.rx()
    if piece then
      radio_session.run(function() radio_session.keys(piece) end)
    end
    uBit.sleep(5)
  end
  listening = false
  radio.close()
end

local peer       -- the board connect() reached
local mid_line   -- what it sent last left a line open on the port

--- The link is closed, and the port and the console's voice
--- go back to the console
local function disconnect()
  radio.close()
  typed_to = to_console
  handler[uBit.DEVICE_ID_RADIO] = nil
  serial_session.send = nil
  write((mid_line and "\n" or "") .. peer .. " disconnected.\n")
  serial_session.prompt()
end

-- What is typed goes over as it is, and the far end echoes and
-- edits it, as a session does with what comes from its port.
-- tx waits to be answered; what is typed meanwhile waits in
-- the port and goes with the next piece. What comes before a
-- Ctrl+D still goes over; what follows it is for the console.
local function to_link(text)
  local before, after = text:match("^([^\4]*)\4(.*)")
  if not before then
    radio.tx(text)
    return
  end
  if #before > 0 then
    radio.tx(before)
  end
  disconnect()
  to_console(after)
end

--- What the link says goes to the port; false is the far end
--- closing it
local function link_to_port()
  local piece = radio.rx()
  if piece then
    serial.send(piece)
    mid_line = piece:sub(-1) ~= "\n"
  elseif piece == false then
    disconnect()
  end
end

function connect(name, timeout)
  radio.enable()
  if not radio.connect(name, timeout) then
    print("Connection timed out.")
    return
  end
  print(name .. " connected.")
  peer = name
  mid_line = false
  typed_to = to_link
  handler[uBit.DEVICE_ID_RADIO] = link_to_port
  serial_session.send = function() end
end

-- Script-level setup (runs once before the main fiber
-- is released):
-- show the prompt, then take what was typed while the
-- script ran, which also arms the port's per-char
-- head-match event. After this returns, release_fiber() in
-- main() hands control to the scheduler; on_event() handles
-- all events from the bus, one at a time.
serial_session.prompt()
handler[uBit.DEVICE_ID_SERIAL](uBit.CODAL_SERIAL_EVT_HEAD_MATCH)
