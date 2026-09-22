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
