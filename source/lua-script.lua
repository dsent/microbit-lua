local uBit = microbit

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

local serial = {
  send = uBit.serial.send,
  getCharAsync = uBit.serial.getCharAsync,
  eventAfterAsync = uBit.serial.eventAfterAsync,
}

-- Output goes wherever the session being served takes it.
-- The serial session names no other way out, so it falls to
-- the port.
local function write(s)
  local session = active_session
  local out = session and session.transport.send
  if out then return out(s) end
  for c in string.gmatch(s, ".") do
    if c == "\n" then
      serial.send("\r")
    end
    serial.send(c)
  end
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

-- A REPL session: a buffer plus the compile-driven submit loop. The
-- same engine is used for the serial console and the BLE UART service.
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
        print("Compile error: " .. tostring(err))
      end
      s.buffer = ""
    end
    collectgarbage("collect")
    s.prompt()
  end
  function s.run(f)
    local saved = active_session
    active_session = s
    f()
    active_session = saved
  end
  return s
end

local serial_session = make_session({
  crlf_before_result = true,
  getChar = serial.getCharAsync,
  arm = function() serial.eventAfterAsync(1) end
})

local handler = { }

microbit.handler = handler

local function enter()
  serial_session.submit("")
end

local function backspace()
  if #serial_session.buffer > 0 then
    serial_session.buffer = serial_session.buffer:sub(1, -2)
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

local function port_to_console(value)
  if value == microbit.CODAL_SERIAL_EVT_HEAD_MATCH then
    serial_session.run(function()
      local c = serial_session.transport.getChar()
      local echo = ""
      while c do
        local input = keypress[c]
        if input then
          if #echo > 0 then
            write(echo)
            echo = ""
          end
          input()
        else
          serial_session.buffer = serial_session.buffer .. c
          echo = echo .. c
        end
        c = serial_session.transport.getChar()
      end
      if #echo > 0 then
        write(echo)
      end
      serial_session.transport.arm()
    end)
  end
end

handler[microbit.DEVICE_ID_SERIAL] = port_to_console

-- TPBot Edu library
-- Based on https://github.com/elecfreaks/pxt-TPBot/blob/master/V2.ts
local getPin = microbit.io.getPin

tpbot = {
  pin_t = getPin(16),
  pin_e = getPin(15)
}

local char = string.char
local i2c_write = microbit.i2c.write

local function send(command, params)
  i2c_write(32, "\255\249"..char(command)..
    char(string.len(params))..params)
end

function tpbot.set_car_light(r, g, b)
  send(48, char(r)..char(g)..char(b))
end

local function abs(x, n)
  return math.abs(x), x < 0 and n or 0
end

local function set_motors_speed(left, right)
  local l, d = abs(left, 1)
  local r, e = abs(right, 2)
  send(16, char(l)..char(r)..char(d + e))
end

tpbot.set_motors_speed = set_motors_speed

function robot_move(left, right, time)
  set_motors_speed(left, right)
  microbit.sleep(1000 * time)
  set_motors_speed(0, 0)
end

local read_digital = microbit.io.getDigitalValue
local pulse_us = microbit.io.pulseUs
local time_pulse_us = microbit.io.getPulseUs

function tpbot.get_distance()
  local e, t = tpbot.pin_e, tpbot.pin_t
  read_digital(e)
  pulse_us(t, 1, 10)
  local r = time_pulse_us(e, 1, 25000)
  return r and r * 0.01715
end

local function hl(x)
  local l = x % 256
  local h = (x - l) / 256
  return char(h)..char(l)
end

function tpbot.run_distance(mm)
  if mm ~= 0 then
    local d, f = abs(mm, 3)
    send(65, hl(d)..char(f))
  end
end

function tpbot.turn(deg)
  if deg ~= 0 then
    local d, f = abs(deg, 1)
    local hl = hl(d)
    send(66, hl..hl..char(f + 1))
  end
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
-- what is typed here goes out, what comes back is printed.

local radio_session = make_session({
  crlf_before_result = false,
  send = function(text) microbit.radio.tx(text) end
})


-- A piece of the link, as much or as little as arrived: what
-- stands before a line ending is entered, what follows it
-- waits for the rest to come.
local function typed(piece)
  local at = string.find(piece, "[\r\n]")
  while at do
    radio_session.buffer =
      radio_session.buffer .. piece:sub(1, at - 1)
    radio_session.submit("")
    piece = piece:sub(at + 1)
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
  microbit.radio.enable()
  while microbit.radio.listen() ~= name do end
  greet()
  while true do
    if microbit.radio.answered() then greet() end
    local piece = microbit.radio.rx()
    if piece then
      radio_session.run(function() typed(piece) end)
    end
    microbit.sleep(5)
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

-- Whoever has the port serves it: the console's own session
-- to start with, the link once connect() has opened one.
-- connect puts the other one in place; nothing asks which.
-- A line at a time goes over the link: tx waits to be
-- answered, and a character each would spend that wait while
-- the next ones pile up in the port. So the typing is echoed
-- as it comes and held until its line is whole.
local function port_to_link(value)
  if value == microbit.CODAL_SERIAL_EVT_HEAD_MATCH then
    local text = typing()
    serial.eventAfterAsync(1)
    write(text)
    typed_here = typed_here .. text
    local at = string.find(typed_here, "[\r\n]")
    while at do
      microbit.radio.tx(typed_here:sub(1, at))
      typed_here = typed_here:sub(at + 1)
      at = string.find(typed_here, "[\r\n]")
    end
  end
end

--- What the link says goes to the port
local function link_to_port()
  local piece = microbit.radio.rx()
  if piece then write(piece) end
end

function connect(name, timeout)
  microbit.radio.enable()
  if not microbit.radio.connect(name, timeout) then
    print("Connection timed out.")
    return
  end
  print(name .. " connected.")
  typed_here = ""
  handler[microbit.DEVICE_ID_SERIAL] = port_to_link
  handler[microbit.DEVICE_ID_RADIO] = link_to_port
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
