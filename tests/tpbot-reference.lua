-- The TPBot commands as source/lua-script.lua held them in Lua at
-- daa2f77 (its lines 282-435, unchanged), before they moved to C in
-- source/tpbot.c. tests/host-tests.sh runs them beside the C and
-- compares what each writes to the bus and says back.

-- TPBot library, for the TPBot Edu and the TPBot Classic
-- Based on https://github.com/elecfreaks/pxt-TPBot (V2.ts for
-- the Edu, V1.ts for the Classic). Like it, every motor and
-- light call sends both robots' commands; each robot ignores
-- the other's.
local getPin = microbit.io.getPin

tpbot = {
  pin_t = getPin(16),
  pin_e = getPin(15)
}

local char = string.char
local i2c_write = microbit.i2c.write

-- Only ever write to the robot: a TPBot Classic that is read
-- from holds the bus until it is switched off and on.
local function to_robot(s)
  if not pcall(i2c_write, 32, s) then
    error("The robot does not answer. Check that it is " ..
      "switched on, or switch it off and on again.", 0)
  end
end

local function send(command, params)
  to_robot("\255\249"..char(command)..
    char(string.len(params))..params)
end

function tpbot.set_car_light(r, g, b)
  local rgb = char(r)..char(g)..char(b)
  send(48, rgb)
  to_robot("\032"..rgb)
end

local function abs(x, n)
  return math.abs(x), x < 0 and n or 0
end

-- A TPBot Classic has been seen letting a wheel creep on after
-- a forward move, when stopped with its reverse bit clear; with
-- the bit set it stays still, so a stopped wheel gets the bit.
-- Both frames are built before either goes out. unanswered
-- says the Edu's frame went to the bus and no robot took it,
-- so no robot moved and none would hear a stop either.
local unanswered = false
local function set_motors_speed(left, right)
  unanswered = false
  local l, d = abs(left, 1)
  local r, e = abs(right, 2)
  local edu = char(l)..char(r)..char(d + e)
  if l < 1 then d = 1 end
  if r < 1 then e = 2 end
  local classic = "\001"..char(l)..char(r)..char(d + e)
  unanswered = true
  send(16, edu)
  unanswered = false
  to_robot(classic)
end

-- Each robot's stop goes out on its own, so one that fails does
-- not keep the other from being tried.
local function stop_motors()
  local edu, err = pcall(send, 16, "\0\0\0")
  local classic, classic_err = pcall(to_robot, "\001\0\0\003")
  if not edu then error(err, 0) end
  if not classic then error(classic_err, 0) end
end

-- Whether a robot answers a command neither one acts on: the
-- Edu's status query, which the Classic ignores. Then which one
-- it is: of the robots tried so far, only the Classic answers an
-- empty write at address 120. An empty write alone proves
-- nothing, since a switched-off Edu seems to take any.
function robot_info()
  if not pcall(i2c_write, 32, "\255\249\160\1\0") then
    return { connected = false }
  end
  local classic = pcall(i2c_write, 240, "")
  return {
    connected = true,
    robot = classic and "TPBot Classic" or "TPBot Edu"
  }
end

tpbot.set_motors_speed = set_motors_speed

-- The motors never start without a time to stop after, and the
-- stop is tried whenever a motor may be running, even when
-- something failed on the way: always, unless no robot answered.
local MAX_SECONDS = 3600

function robot_move(left, right, time)
  if type(time) ~= "number"
    or not (time >= 0 and time <= MAX_SECONDS) then
    error("robot_move needs how many seconds to drive, from 0 " ..
      "to " .. MAX_SECONDS .. ", such as robot_move(50, 50, 1).", 0)
  end
  local moved, err = pcall(function()
    set_motors_speed(left, right)
    microbit.sleep(1000 * time)
  end)
  local stopped, stop_err = true, nil
  if moved or not unanswered then
    stopped, stop_err = pcall(stop_motors)
  end
  if not moved then error(err, 0) end
  if not stopped then error(stop_err, 0) end
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
