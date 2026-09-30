local uBit = require("microbit")
require("microbit.audio")
require("microbit.display")
local radio = require("microbit.radio")
local serial = require("microbit.serial")
local tpbot = require("tpbot2")

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

local to_string = tostring

local PRECISION = 1e-6

local function fraction(o, i, f)
  local p = PRECISION
  while f >= p do
    i, f = math.modf(10 * f)
    table.insert(o, string.format("%d", i))
    p = 10 * p
  end
end

local function number2str(n)
  local i, f = math.modf(n)
  local o = {
    string.format("%d", i)
  }
  if f >= PRECISION then
    table.insert(o, ".")
    fraction(o, i, f)
  end
  return table.concat(o)
end

function tostring(o)
  if type(o) ~= "number" then
    return to_string(o)
  end
  return number2str(o)
end

collectgarbage("setpause", 100)
print("micro:bit\nLua 5.1 REPL")

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
  --- line and is echoed; the key after it erases or enters
  function s.keys(text)
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

microbit.handler = handler

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

-- A handler for the port that gives what is typed to take, a
-- piece at a time, and watches the port again only once it is
-- empty: a piece is dealt with before the next one is taken.
local function port_to(take)
  return function(value)
    if value == microbit.CODAL_SERIAL_EVT_HEAD_MATCH then
      local text = typing()
      while #text > 0 do
        take(text)
        text = typing()
      end
      serial.eventAfterAsync(1)
    end
  end
end

local port_to_console = port_to(function(text)
  serial_session.run(function() serial_session.keys(text) end)
end)

handler[microbit.DEVICE_ID_SERIAL] = port_to_console

function robot_move(left, right, time)
  tpbot.set_motors_speed(left, right)
  microbit.sleep(1000 * time)
  tpbot.set_motors_speed(0, 0)
end

function turn(h)
  h = h % 12
  if 6 < h then
    h = h - 12
  end
  tpbot.turn(-30 * h)
end

function straight(l)
  tpbot.run_distance(110 * l)
end

local function button(value, btn)
  if value == microbit.DEVICE_BUTTON_EVT_CLICK then
      uBit.display.scroll(btn)
   elseif value == microbit.DEVICE_BUTTON_EVT_LONG_CLICK then
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

function on_event(source, value, timestamp)
  local handle = handler[source]
  if handle then
    handle(value, timestamp)
  end
end


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
-- what is typed here goes out, what comes back goes on the
-- port as it is, and the console here says no more.

local radio_session = make_session(radio.tx)


--- A fresh session for a caller that has just arrived
local function greet()
  radio_session.buffer = ""
  radio_session.run(radio_session.prompt)
end

function listen(name)
  radio.enable()
  radio.listen(name)
  greet()
  while true do
    if radio.answered(name) then greet() end
    local piece = radio.rx()
    if piece then
      radio_session.run(function() radio_session.keys(piece) end)
    end
    microbit.sleep(5)
  end
end

-- Whoever has the port serves it: the console's own session
-- to start with, the link once connect() has opened one.
-- connect puts the other one in place; nothing asks which.
-- What is typed goes over as it is, and the far end echoes
-- and edits it, as a session does with what comes from its
-- port. tx waits to be answered; what is typed meanwhile
-- waits in the port and goes with the next piece.
local port_to_link = port_to(radio.tx)

--- What the link says goes to the port
local function link_to_port()
  local piece = radio.rx()
  if piece then serial.send(piece) end
end

function connect(name, timeout)
  radio.enable()
  if not radio.connect(name, timeout) then
    print("Connection timed out.")
    return
  end
  print(name .. " connected.")
  handler[microbit.DEVICE_ID_SERIAL] = port_to_link
  handler[microbit.DEVICE_ID_RADIO] = link_to_port
  serial_session.send = function() end
end

-- Script-level setup (runs once before the main fiber
-- is released):
-- show prompt, initialise the serial RX buffer, and arm
-- the first per-char head-match event on both transports.
-- After this returns, release_fiber() in main() hands
-- control to the scheduler; on_event() handles all events 
-- from the bus.
serial_session.prompt()
serial.getCharAsync()
serial.eventAfterAsync(1)
