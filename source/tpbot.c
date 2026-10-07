// -*- mode: c; indent-tabs-mode: nil; -*-
//
// TPBot commands, for the TPBot Edu and the TPBot Classic
//
// Based on https://github.com/elecfreaks/pxt-TPBot (V2.ts for the Edu,
// V1.ts for the Classic). Like it, every motor and light call sends both
// robots' commands; each robot ignores the other's.
//
// These were Lua in source/lua-script.lua; tests/tpbot-reference.lua keeps
// that Lua. Every byte on the bus, every value returned and every message
// is the Lua's, and tests/host-tests.sh runs the two side by side to
// show it. The arithmetic is written as Lua's VM does it on lua_Number, and
// a mistake in a call reads as the Lua's did, save for the position the
// Lua put in front of a few.

#include <math.h>

#include "lua.h"
#include "lauxlib.h"
#include "tpbot.h"

// Each helper below returns 0, or 1 with the message it stops on pushed.

static int stop_with(lua_State *L, const char *message) {
  lua_pushstring(L, message);
  return 1;
}

// A message said as it stands, with no position in front.
static int say(lua_State *L, const char *message) {
  lua_pushstring(L, message);
  return lua_error(L);
}

const char tpbot_no_answer[] =
  "The robot does not answer. Check that it is "
  "switched on, or switch it off and on again.";

// string.char's check of its argument, as the Lua called it: through an
// upvalue whose name the stripped script no longer knows.
static int byte_of(lua_State *L, lua_Number n, char *out) {
  lua_Integer i;
  int c;
  lua_number2integer(i, n);
  c = (int)i;
  if ((unsigned char)c != c)
    return stop_with(L, "bad argument #1 to '?' (invalid value)");
  *out = (char)c;
  return 0;
}

static int byte_at(lua_State *L, int arg, char *out) {
  if (!lua_isnumber(L, arg)) {
    lua_pushfstring(L, "bad argument #1 to '?' (number expected, got %s)",
                    luaL_typename(L, arg));
    return 1;
  }
  return byte_of(L, lua_tonumber(L, arg), out);
}

// abs(x, n): math.abs(x), then n when x < 0, else 0.
static int abs_at(lua_State *L, int arg, int n,
                  lua_Number *magnitude, int *flag) {
  lua_Number x;
  if (!lua_isnumber(L, arg)) {
    lua_pushfstring(L, "bad argument #1 to 'abs' (number expected, got %s)",
                    luaL_typename(L, arg));
    return 1;
  }
  x = lua_tonumber(L, arg);
  *magnitude = (lua_Number)fabs(x);
  if (lua_type(L, arg) != LUA_TNUMBER)
    return stop_with(L, "attempt to compare string with number");
  *flag = x < 0 ? n : 0;
  return 0;
}

// Only ever write to the robot: a TPBot Classic that is read from holds
// the bus until it is switched off and on.
static int to_robot(const char *frame, size_t length) {
  return tpbot_i2c_write(32, frame, length) != 0;
}

// The Edu's frame: 255, 249, the command, the parameters' count, then
// the parameters.
static int send(int command, const char *params, int count) {
  char frame[4 + 5];
  int i;
  (void)sizeof(char[sizeof frame <= TPBOT_FRAME_MAX ? 1 : -1]);
  frame[0] = (char)255;
  frame[1] = (char)249;
  frame[2] = (char)command;
  frame[3] = (char)count;
  for (i = 0; i < count; i++)
    frame[4 + i] = params[i];
  return to_robot(frame, 4 + count);
}

// x in two bytes, high then low.
static int two_bytes(lua_State *L, lua_Number x, char *out) {
  lua_Number l = x - floor(x / (lua_Number)256) * (lua_Number)256;
  lua_Number h = (x - l) / (lua_Number)256;
  if (byte_of(L, h, &out[0]) || byte_of(L, l, &out[1]))
    return 1;
  return 0;
}

int tpbot_set_car_light(lua_State *L) {
  char frame[4];
  lua_settop(L, 3);
  if (byte_at(L, 1, &frame[1]) || byte_at(L, 2, &frame[2])
      || byte_at(L, 3, &frame[3]))
    return lua_error(L);
  if (send(48, frame + 1, 3))
    return say(L, tpbot_no_answer);
  frame[0] = 32;
  if (to_robot(frame, 4))
    return say(L, tpbot_no_answer);
  return 0;
}

// A TPBot Classic has been seen letting a wheel creep on after a forward
// move, when stopped with its reverse bit clear; with the bit set it stays
// still, so a stopped wheel gets the bit. Both frames are built before
// either goes out. unanswered says the Edu's frame went to the bus and no
// robot took it, so no robot moved and none would hear a stop either.
//
// 1 is a mistake in the speeds, found before anything goes out, with its
// message pushed; NO_ANSWER, a frame no robot took, with nothing pushed:
// a motor may be running, and making the message could run out of memory
// before the stop goes out.
#define NO_ANSWER 2
static int unanswered = 0;

static int set_motors_speed(lua_State *L, int left, int right) {
  lua_Number l, r;
  int d, e;
  char edu[3], classic[4];
  unanswered = 0;
  if (abs_at(L, left, 1, &l, &d) || abs_at(L, right, 2, &r, &e))
    return 1;
  if (byte_of(L, l, &edu[0]) || byte_of(L, r, &edu[1])
      || byte_of(L, (lua_Number)(d + e), &edu[2]))
    return 1;
  if (l < 1) d = 1;
  if (r < 1) e = 2;
  classic[0] = 1;
  classic[1] = edu[0];
  classic[2] = edu[1];
  classic[3] = (char)(d + e);
  unanswered = 1;
  if (send(16, edu, 3))
    return NO_ANSWER;
  unanswered = 0;
  if (to_robot(classic, 4))
    return NO_ANSWER;
  return 0;
}

int tpbot_set_motors_speed(lua_State *L) {
  int failed;
  lua_settop(L, 2);
  failed = set_motors_speed(L, 1, 2);
  if (failed == NO_ANSWER)
    return say(L, tpbot_no_answer);
  if (failed)
    return lua_error(L);
  return 0;
}

// Each robot's stop goes out on its own, so one that fails does not keep
// the other from being tried.
static int stop_motors(void) {
  int edu = send(16, "\x00\x00\x00", 3);
  int classic = to_robot("\x01\x00\x00\x03", 4);
  return edu || classic;
}

// Whether a robot answers a command neither one acts on: the Edu's status
// query, which the Classic ignores. Then which one it is: of the robots
// tried so far, only the Classic answers an empty write at address 120.
// An empty write alone proves nothing, since a switched-off Edu seems to
// take any.
int tpbot_robot_info(lua_State *L) {
  int classic;
  if (tpbot_i2c_write(32, "\xff\xf9\xa0\x01\x00", 5) != 0) {
    lua_createtable(L, 0, 1);
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "connected");
    return 1;
  }
  classic = tpbot_i2c_write(240, "", 0) == 0;
  lua_createtable(L, 0, 2);
  lua_pushboolean(L, 1);
  lua_setfield(L, -2, "connected");
  lua_pushstring(L, classic ? "TPBot Classic" : "TPBot Edu");
  lua_setfield(L, -2, "robot");
  return 1;
}

// The motors never start without a time to stop after, and the stop is
// tried whenever a motor may be running, even when something failed on
// the way: always, unless no robot answered. Between the start and the
// stop nothing can raise an error: the sleep's look for on_event is
// protected, and a message is made only once both stops have gone out.
#define MAX_SECONDS 3600
#define STRING(x) #x
#define TEXT(x) STRING(x)

int tpbot_robot_move(lua_State *L) {
  lua_Number time;
  int failed, stop_failed = 0;
  lua_settop(L, 3);
  time = lua_tonumber(L, 3);
  if (lua_type(L, 3) != LUA_TNUMBER
      || !(time >= 0 && time <= MAX_SECONDS))
    return say(L, "robot_move needs how many seconds to drive, "
               "from 0 to " TEXT(MAX_SECONDS)
               ", such as robot_move(50, 50, 1).");
  failed = set_motors_speed(L, 1, 2);
  if (!failed) {
    lua_Integer ms;
    lua_number2integer(ms, (lua_Number)1000 * time);
    tpbot_sleep((uint32_t)ms);
  }
  if (!failed || !unanswered)
    stop_failed = stop_motors();
  if (failed == NO_ANSWER)
    return say(L, tpbot_no_answer);
  if (failed)
    return lua_error(L);
  if (stop_failed)
    return say(L, tpbot_no_answer);
  return 0;
}

// Measured by polling the echo pin: getPulseUs would flood the event
// handler with PulseIn events.
int tpbot_get_distance(lua_State *L) {
  int width = tpbot_echo_us();
  if (width < 0)
    lua_pushnil(L);
  else
    lua_pushnumber(L, (lua_Number)width * (lua_Number)0.01715);
  return 1;
}

// A number, as the Lua compared it with 0: another type never equals it.
static int is_zero(lua_State *L, int arg) {
  return lua_type(L, arg) == LUA_TNUMBER && lua_tonumber(L, arg) == 0;
}

static int run_distance(lua_State *L, int arg) {
  lua_Number d;
  int f;
  char params[3];
  if (is_zero(L, arg))
    return 0;
  if (abs_at(L, arg, 3, &d, &f) || two_bytes(L, d, params)
      || byte_of(L, (lua_Number)f, &params[2]))
    return 1;
  if (send(65, params, 3))
    return stop_with(L, tpbot_no_answer);
  return 0;
}

int tpbot_run_distance(lua_State *L) {
  lua_settop(L, 1);
  if (run_distance(L, 1))
    return lua_error(L);
  return 0;
}

static int turn(lua_State *L, int arg) {
  lua_Number d;
  int f;
  char params[5];
  if (is_zero(L, arg))
    return 0;
  if (abs_at(L, arg, 1, &d, &f) || two_bytes(L, d, params)
      || byte_of(L, (lua_Number)(f + 1), &params[4]))
    return 1;
  params[2] = params[0];
  params[3] = params[1];
  if (send(66, params, 5))
    return stop_with(L, tpbot_no_answer);
  return 0;
}

int tpbot_turn(lua_State *L) {
  lua_settop(L, 1);
  if (turn(L, 1))
    return lua_error(L);
  return 0;
}

// A number for arithmetic, as Lua's VM takes one: numbers, and strings
// that read as one.
static int arithmetic_on(lua_State *L, int arg, lua_Number *out) {
  if (!lua_isnumber(L, arg)) {
    lua_pushfstring(L, "attempt to perform arithmetic on a %s value",
                    luaL_typename(L, arg));
    return 1;
  }
  *out = lua_tonumber(L, arg);
  return 0;
}

// Whether a value can be indexed: a table, or one whose metatable says how
static int indexable(lua_State *L, int at) {
  if (lua_istable(L, at))
    return 1;
  if (luaL_getmetafield(L, at, "__index")) {
    lua_pop(L, 1);
    return 1;
  }
  return 0;
}

static int callable(lua_State *L, int at) {
  if (lua_isfunction(L, at))
    return 1;
  if (luaL_getmetafield(L, at, "__call")) {
    lua_pop(L, 1);
    return 1;
  }
  return 0;
}

// tpbot.<name>, pushed, looked up at the call as the Lua looked it up, so
// that a tpbot.turn of a person's own is the one turn() calls.
static int command(lua_State *L, const char *name) {
  lua_getglobal(L, "tpbot");
  if (!indexable(L, -1)) {
    lua_pushfstring(L, "attempt to index global 'tpbot' (a %s value)",
                    luaL_typename(L, -1));
    return 1;
  }
  lua_getfield(L, -1, name);
  lua_remove(L, -2);
  return 0;
}

static int call_command(lua_State *L, const char *name) {
  if (!callable(L, -2)) {
    lua_pushfstring(L, "attempt to call field '%s' (a %s value)", name,
                    luaL_typename(L, -2));
    return 1;
  }
  lua_call(L, 1, 0);
  return 0;
}

// turn(h): h hours on a clock face, the shorter way round.
int tpbot_turn_hours(lua_State *L) {
  lua_Number h;
  lua_settop(L, 1);
  if (arithmetic_on(L, 1, &h))
    return lua_error(L);
  h = h - floor(h / (lua_Number)12) * (lua_Number)12;
  if (6 < h)
    h = h - 12;
  if (command(L, "turn"))
    return lua_error(L);
  lua_pushnumber(L, (lua_Number)-30 * h);
  if (call_command(L, "turn"))
    return lua_error(L);
  return 0;
}

// straight(l): l squares of 14.5 cm.
int tpbot_straight(lua_State *L) {
  lua_Number l;
  lua_settop(L, 1);
  if (command(L, "run_distance") || arithmetic_on(L, 1, &l))
    return lua_error(L);
  lua_pushnumber(L, (lua_Number)145 * l);
  if (call_command(L, "run_distance"))
    return lua_error(L);
  return 0;
}

void tpbot_register_globals(lua_State *L) {
#define X(name, function) lua_register(L, #name, function);
  TPBOT_GLOBALS
#undef X
}
