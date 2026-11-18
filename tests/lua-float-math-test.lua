-- Lua's numbers and math in float, as the firmware builds them
-- (LUA_NUMBER_IS_FLOAT and source/lua-float-math.h)
local checks, failures = 0, 0
local function check(ok, what)
  checks = checks + 1
  if not ok then failures = failures + 1 end
  print((ok and "ok   " or "FAIL ") .. what)
end

check(2^24 + 1 == 2^24, "numbers are floats: 2^24 + 1 rounds to 2^24")
local two, half, a, b = 2, 0.5, 7.3, 3
check(2^0.5 == two^half, "^ gives the same folded by the compiler and run")
check(7.3 % 3 == a % b, "% gives the same folded by the compiler and run")
check(2^10 == 1024 and two^-1 == 0.5, "^ with a whole exponent is exact")
check(5.5 % 2 == 1.5 and -5 % 3 == 1 and 5 % -3 == -1,
      "% takes the sign of the divisor")
local big, three, huge, minus = 16777228, 3, 3e38, -2e38
check(16777228 % 3 == 1 and big % three == 1,
      "% keeps its product in double: 16777228 % 3 is 1")
local r1, r2 = 3e38 % -2e38, huge % minus
check(r1 == r2 and r1 < -9e37 and r1 > -2e38,
      "% keeps its product in double: 3e38 % -2e38 is finite")
check(math.floor(-0.5) == -1 and math.ceil(-0.5) == 0, "floor and ceil")
check(math.fmod(7, 3) == 1 and math.fmod(-7, 3) == -1,
      "fmod takes the sign of the dividend")
local ip, fp = math.modf(3.75)
check(ip == 3 and fp == 0.75, "modf parts")
local m, e = math.frexp(12)
check(m == 0.75 and e == 4 and math.ldexp(0.75, 4) == 12, "frexp and ldexp")
check(math.huge == math.huge * 2 and -math.huge < -2^127,
      "math.huge is infinite")
check(math.abs(math.sin(math.pi)) < 1e-6 and math.cos(0) == 1
      and math.sqrt(16) == 4 and math.exp(0) == 1 and math.log10(1000) == 3,
      "sin, cos, sqrt, exp and log10 at exact points")
check(math.abs(math.atan2(1, 1) - math.pi / 4) < 1e-6, "atan2")
local r = math.random(5)
check(r >= 1 and r <= 5 and r == math.floor(r), "random(5) is whole, 1 to 5")
print(checks .. " float math checks, " .. failures .. " failed")
os.exit(failures == 0 and 0 or 1)
