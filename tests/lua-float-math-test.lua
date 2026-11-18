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
check(16777228 % 3 == 1 and big % three == 1, "% is exact: 16777228 % 3 is 1")
local r1, r2 = 3e38 % -2e38, huge % minus
check(r1 == r2 and r1 < -9e37 and r1 > -2e38,
      "% stays finite: 3e38 % -2e38 is about -1e38")
local n7, seven, n865, big8 = 57357096, 7, 865, 8365487616
check(57357096 % 7 == 6 and n7 % seven == 6 and -n7 % seven == 1
      and n7 % -seven == -1,
      "% is exact: 57357096 % 7 is 6, not -1")
check(8365487616 % 865 == 821 and big8 % n865 == 821,
      "% is exact: 8365487616 % 865 is 821")
local two59, n49 = 2^59, 49
check(2^59 % 49 == 46 and two59 % n49 == 46, "% is exact: 2^59 % 49 is 46")
-- made at run time: the constant table would merge -0 with an earlier 0
local z, six, inf = -math.abs(0), 6, math.huge
check(1 / z == -inf and 1 / (z % 3) == inf and 1 / (-six % 3) == inf,
      "% gives a zero as +0")
local m1, m2 = 5 % inf, inf % 5
check(m1 ~= m1 and m2 ~= m2, "% by or of an infinity is nan, as it was")
local outside = 0
for k = 1, 3000 do
  local x, d = 1e7 + k * 3331.75, 3 + k % 997
  local m = (x * k) % d
  if m < 0 or m >= d then outside = outside + 1 end
end
check(outside == 0, "% of big numbers stays from 0 to below the divisor")
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
-- tests/lua-random-stub.c pins rand() for seeds 31 to 36
math.randomseed(31)
check(math.random() < 1, "random() stays below 1 for rand() near RAND_MAX")
check(math.random(10) == 10 and math.random(3, 5) == 5,
      "random(n) stays at n or below for rand() near RAND_MAX")
local w = math.random(-1073741824, 1073741824)
check(w >= -1073741824 and w <= 1073741824,
      "a range wider than an int's half stays in range")
check(math.random(-16777216, 0) == 0, "a range's top is reachable: 0")
math.randomseed(32)
check(math.random(33554432) == 2, "a wide range keeps small results: 2")
math.randomseed(33)
check(math.random(6) == 6, "random(6) is exact: 6, not 5")
math.randomseed(34)
check(math.random(6) == 5, "random(6) is exact: 5, not 6")
math.randomseed(35)
check(math.random(-2147483392, 255) == 0, "an int-wide range is exact: 0")
math.randomseed(36)
check(math.random() < 1 and math.random(10) == 10
      and math.random(-2147483392, 255) == 255,
      "the top draw stays below 1 and reaches a 2^31-wide range's top")
math.randomseed(1)
print(checks .. " float math checks, " .. failures .. " failed")
os.exit(failures == 0 and 0 or 1)
