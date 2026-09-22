-- TPBot Classic library
local getPin = microbit.io.getPin

tpbot = {
  pin_t = getPin(16),
  pin_e = getPin(15)
}

local char = string.char
local i2c_write = microbit.i2c.write

local function send(s)
  i2c_write(32, s)
end

function tpbot.set_car_light(r, g, b)
  send("\032"..char(r)..char(g)..char(b))
end

local function abs(x, n)
  return math.abs(x), x < 0 and n or 0
end

local function set_motors_speed(left, right)
  local l, d = abs(left, 1)
  local r, e = abs(right, 2)
  send("\001"..char(l)..char(r)..char(d + e))
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
