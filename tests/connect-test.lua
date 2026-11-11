-- connect()'s calls after a lost link, on the host: the firmware's own
-- script runs over a stand-in board whose radio and serial port the test
-- drives. Run by tests/connect-tests.sh: lua connect-test.lua SCRIPT
local script = assert(arg[1], "usage: lua connect-test.lua lua-script.lua")
local say = print -- the script puts its own print in place
local failed, checks = 0, 0

local function check(ok, what)
  checks = checks + 1
  if not ok then
    failed = failed + 1
    say("FAIL: " .. what)
  end
end

-- The stand-in board. A name in capitals is a constant with a number of
-- its own; any other function the script calls does nothing.
local out, keys, calls, answers, sent, rx_queue = {}, {}, {}, {}, {}, {}
local on_call -- run at each call: what is typed while it waits stays in
-- the port, since no handler runs until the calls end (#40's engine)
local closes = 0
local on_sleep -- run once at the next sleep, as an event the sleep handles

local function quiet(t)
  return setmetatable(t, {
    __index = function() return function() end end
  })
end

local next_id = 100
local uBit = setmetatable({
  friendlyName = function() return "zopoz" end,
  version = function() return "host" end,
  accelerometer = quiet({}),
  sleep = function()
    local f = on_sleep
    on_sleep = nil
    if f then f() end
  end,
}, {
  __index = function(t, k)
    if k:match("^[A-Z][A-Z0-9_]*$") then
      next_id = next_id + 1
      rawset(t, k, next_id)
      return next_id
    end
  end
})

local serial = quiet({
  send = function(s) out[#out + 1] = s end,
  getCharAsync = function() return table.remove(keys, 1) end,
})

local radio = quiet({
  connect = function(name)
    calls[#calls + 1] = name
    -- no test makes this many: a loop that does not end fails, not hangs
    if #calls > 50 then error("more than 50 calls", 0) end
    if on_call then on_call(#calls) end
    return table.remove(answers, 1) or false
  end,
  close = function() closes = closes + 1 end,
  rx = function()
    local r = table.remove(rx_queue, 1)
    if r then return r[1], r[2] end
    return nil
  end,
  tx = function(s)
    sent[#sent + 1] = s
    return true
  end,
})

package.preload["microbit"] = function() return uBit end
package.preload["microbit.accelerometer"] = function() return quiet({}) end
package.preload["microbit.io"] = function() return quiet({}) end
package.preload["microbit.audio"] = function() return quiet({}) end
package.preload["microbit.display"] = function() return quiet({}) end
package.preload["microbit.radio"] = function() return radio end
package.preload["microbit.serial"] = function() return serial end

assert(loadfile(script))()

local function output()
  local s = table.concat(out)
  out = {}
  return s
end

local function has(s, part)
  return s:find(part, 1, true) ~= nil
end

local function type_keys(text)
  for i = 1, #text do keys[#keys + 1] = text:sub(i, i) end
  on_event(uBit.DEVICE_ID_SERIAL, uBit.CODAL_SERIAL_EVT_HEAD_MATCH)
end

local function radio_event()
  on_event(uBit.DEVICE_ID_RADIO, 1)
end

-- Connected to puzup, with tries as connect's third argument
local function linked(tries)
  out, keys, calls, sent, rx_queue, on_call = {}, {}, {}, {}, {}, nil
  answers = { true }
  connect("puzup", nil, tries)
  return output()
end

-- A lost link with the default: the calls that fail, then the one that
-- answers, and the port goes to the link again; keys typed meanwhile are
-- dropped
check(has(linked(), "puzup connected."), "connect answers")
answers = { false, false, true }
on_call = function(n) if n == 2 then keys[#keys + 1] = "a" end end
rx_queue = { { false, false } }
radio_event()
local o = output()
check(has(o, "puzup lost; calling again, Ctrl+D stops."),
      "a lost link says it calls again: " .. o)
check(has(o, "puzup connected."), "and says when it is back: " .. o)
check(#calls == 4, "three calls after the first: " .. #calls)
type_keys("x")
check(sent[#sent] == "x", "the port goes to the link again")
check(not has(table.concat(sent), "a"), "a key typed while calling is dropped")

-- 0: a lost link is over, as before
linked(0)
rx_queue = { { false, false } }
radio_event()
o = output()
check(has(o, "puzup disconnected."), "0: a lost link is over: " .. o)
check(not has(o, "calling again"), "0: no call again")
check(#calls == 1, "0: no call after the first: " .. #calls)

-- 1 and 3: that many calls, then the link is over
for _, n in ipairs({ 1, 3 }) do
  linked(n)
  rx_queue = { { false, false } }
  radio_event()
  o = output()
  check(#calls == 1 + n, n .. ": " .. n .. " calls: " .. #calls - 1)
  check(has(o, "puzup disconnected."), n .. ": then over: " .. o)
end

-- The count is per lost link: 2, the second call answers; the next loss
-- gets 2 calls again
linked(2)
answers = { false, true }
rx_queue = { { false, false } }
radio_event()
check(has(output(), "puzup connected."), "2: back at the second call")
rx_queue = { { false, false } }
radio_event()
o = output()
check(#calls == 5, "2: two calls for each loss: " .. #calls - 1)
check(has(o, "puzup disconnected."), "2: over after the second loss")

-- Ctrl+D while it calls with the default: the calls end, and the port
-- is the console's again
linked()
on_call = function(n) if n == 3 then keys[#keys + 1] = "\4" end end
rx_queue = { { false, false } }
radio_event()
o = output()
check(#calls == 3, "Ctrl+D ends the calls: " .. #calls - 1 .. " made")
check(has(o, "puzup disconnected."), "Ctrl+D: over: " .. o)
type_keys("6*7\r")
check(has(output(), "=> 42"), "Ctrl+D: the console answers again")

-- A link the far end closes is over at once, whatever the attempts
linked()
rx_queue = { { false, true } }
radio_event()
o = output()
check(#calls == 1, "closed: no call: " .. #calls - 1)
check(has(o, "puzup disconnected."), "closed: over: " .. o)

-- What comes over the link before the loss still reaches the port
linked()
answers = { true }
rx_queue = { { "hello" }, { false, false } }
radio_event()
radio_event()
o = output()
check(has(o, "hello\r\npuzup lost"), "an open line ends before the notice: " .. o)
check(#calls == 2 and has(o, "puzup connected."),
      "a Ctrl+D before does not stop the next calls: " .. o)

-- A Ctrl+D typed during a call that is answered: the new link is closed
-- again, and the console has the port
linked()
answers = { true }
closes = 0
on_call = function(n) if n == 2 then keys[#keys + 1] = "\4" end end
rx_queue = { { false, false } }
radio_event()
o = output()
check(closes >= 1, "Ctrl+D during an answered call closes the link again")
check(not has(o, "puzup connected."), "Ctrl+D: not back on the link: " .. o)
on_call = nil
type_keys("6*7\r")
check(has(output(), "=> 42"), "Ctrl+D during an answered call: console")

-- Keys typed during a call are dropped, the answered call's as well as
-- the last one's
linked()
answers = { false, true }
on_call = function(n)
  if n == 2 then keys[#keys + 1] = "q" end
  if n == 3 then keys[#keys + 1] = "w" end
end
rx_queue = { { false, false } }
radio_event()
check(has(output(), "puzup connected."), "dropped keys: back on the link")
on_call = nil
type_keys("x")
local tx = table.concat(sent)
check(tx == "x", "keys typed while calling are not sent: " .. tx)
linked(1)
on_call = function() for c in ("6*7\r"):gmatch(".") do keys[#keys + 1] = c end end
rx_queue = { { false, false } }
radio_event()
on_call = nil
type_keys("")
check(not has(output(), "=> 42"), "keys typed during the last call are dropped")

-- A loss handled while a line at the console sleeps, which connect()
-- left serving the console with its output muted: the notices still
-- reach the port
out, keys, calls, sent, rx_queue, on_call = {}, {}, {}, {}, {}, nil
answers = { true, false, true }
rx_queue = { { false, false } }
on_sleep = radio_event
type_keys('connect("puzup") require("microbit").sleep(10)\r')
o = output()
check(has(o, "puzup lost; calling again, Ctrl+D stops."),
      "a loss during a line's sleep is said: " .. o)
local _, links = o:gsub("puzup connected%.", "")
check(links == 2, "... and the link back, after connect()'s own: " .. o)

-- A first call that is not answered times out, as before
out, calls, answers = {}, {}, { false }
connect("puzup")
check(has(output(), "Connection timed out."), "a first call times out")
check(#calls == 1, "a first call is made once: " .. #calls)

-- attempts must be a number
local ok, err = pcall(connect, "puzup", nil, "x")
check(not ok and has(tostring(err), "attempts must be a number"),
      "a wrong attempts is refused: " .. tostring(err))

say(checks .. " checks, " .. failed .. " failed")
os.exit(failed == 0 and 0 or 1)
