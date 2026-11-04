// -*- mode: c; indent-tabs-mode: nil; -*-
//
// TPBot commands, for the TPBot Classic and the TPBot Edu
//
// Based on https://github.com/elecfreaks/pxt-TPBot (V1.ts for the Classic,
// V2.ts for the Edu). The tpbot module holds what both robots do, and each
// of its commands sends both robots' frames, as pxt-TPBot does; each robot
// ignores the other's, so one program drives either. The tpbot2 module
// holds everything the Edu does and sends the Edu's frames only. Both
// share the functions below, told by `classic` whether the Classic's
// frames go out too.

#include <math.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "tpbot.h"

const char tpbot_no_answer[] =
  "The robot does not answer. Check that it is "
  "switched on, or switch it off and on again.";

// A message to the person at the robot, said with no position in front
static int say(lua_State *L, const char *message) {
  lua_pushstring(L, message);
  return lua_error(L);
}

static int no_answer(lua_State *L) {
  return say(L, tpbot_no_answer);
}

// Only ever write to the robot: a TPBot Classic that is read from holds
// the bus until it is switched off and on. Nonzero when no robot took it.
static int to_robot(const char *frame, size_t length) {
  return tpbot_i2c_write(32, frame, length) != 0;
}

// The Edu's frame: 255, 249, the command, the parameters' count, then
// the parameters.
static int to_edu(int command, const char *params, int count) {
  char frame[4 + 5];
  int i;
  frame[0] = (char)255;
  frame[1] = (char)249;
  frame[2] = (char)command;
  frame[3] = (char)count;
  for (i = 0; i < count; i++)
    frame[4 + i] = params[i];
  return to_robot(frame, 4 + count);
}

static char byte_at(lua_State *L, int arg, int value) {
  luaL_argcheck(L, 0 <= value && value <= 255, arg, "out of range");
  return (char)value;
}

static int set_car_light(lua_State *L, int classic) {
  char frame[4];
  int i;
  frame[0] = 32;
  for (i = 1; i <= 3; i++)
    frame[i] = byte_at(L, i, luaL_checkint(L, i));
  if (to_edu(48, frame + 1, 3) || (classic && to_robot(frame, 4)))
    return no_answer(L);
  return 0;
}

int tpbot_set_car_light(lua_State *L) { return set_car_light(L, 1); }
int tpbot2_set_car_light(lua_State *L) { return set_car_light(L, 0); }

// Both robots' frames for the speeds at arg and arg + 1, -255 to 255,
// checked before anything goes out. A TPBot Classic has been seen letting
// a wheel creep on after a forward move when stopped with its reverse bit
// clear; with the bit set it stays still, so a stopped wheel gets it.
static void speed_frames(lua_State *L, int arg, char edu[3],
                         char classic[4]) {
  int left = luaL_checkint(L, arg), right = luaL_checkint(L, arg + 1);
  edu[0] = byte_at(L, arg, abs(left));
  edu[1] = byte_at(L, arg + 1, abs(right));
  edu[2] = (char)((left < 0) + 2 * (right < 0));
  classic[0] = 1;
  classic[1] = edu[0];
  classic[2] = edu[1];
  classic[3] = (char)((left <= 0) + 2 * (right <= 0));
}

static int set_motors_speed(lua_State *L, int classic) {
  char edu[3], frame[4];
  speed_frames(L, 1, edu, frame);
  if (to_edu(16, edu, 3) || (classic && to_robot(frame, 4)))
    return no_answer(L);
  return 0;
}

int tpbot_set_motors_speed(lua_State *L) { return set_motors_speed(L, 1); }
int tpbot2_set_motors_speed(lua_State *L) { return set_motors_speed(L, 0); }

// Each robot's stop goes out on its own, so one that fails does not keep
// the other from being tried.
static int stop_motors(int classic) {
  int failed = to_edu(16, "\0\0\0", 3);
  if (classic)
    failed |= to_robot("\x01\x00\x00\x03", 4);
  return failed;
}

// The motors never start without a time to stop after. Once a frame has
// been taken, a motor may be running, so the stops go out before any
// message is made. When the first frame is not taken, no robot is there
// and none would hear a stop either.
#define MAX_SECONDS 3600
#define STRING(x) #x
#define TEXT(x) STRING(x)

static int robot_move(lua_State *L, int classic) {
  char edu[3], frame[4];
  lua_Number seconds = lua_tonumber(L, 3);
  int moved;
  if (!lua_isnumber(L, 3) || !(seconds >= 0 && seconds <= MAX_SECONDS))
    return say(L, "robot_move needs how many seconds to drive, "
               "from 0 to " TEXT(MAX_SECONDS)
               ", such as robot_move(50, 50, 1).");
  speed_frames(L, 1, edu, frame);
  if (to_edu(16, edu, 3))
    return no_answer(L);
  moved = !classic || !to_robot(frame, 4);
  if (moved)
    tpbot_sleep((uint32_t)(seconds * 1000 + 0.5f));
  if (stop_motors(classic) || !moved)
    return no_answer(L);
  return 0;
}

int tpbot_robot_move(lua_State *L) { return robot_move(L, 1); }
int tpbot2_robot_move(lua_State *L) { return robot_move(L, 0); }

// Whether a robot answers a command neither one acts on: the Edu's status
// query, which the Classic ignores. Then which one it is: of the robots
// tried so far, only the Classic answers an empty write at address 120.
// An empty write alone proves nothing, since a switched-off Edu seems to
// take any.
int tpbot_robot_info(lua_State *L) {
  int classic;
  lua_createtable(L, 0, 2);
  if (tpbot_i2c_write(32, "\xff\xf9\xa0\x01\x00", 5) != 0) {
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "connected");
    return 1;
  }
  classic = tpbot_i2c_write(240, "", 0) == 0;
  lua_pushboolean(L, 1);
  lua_setfield(L, -2, "connected");
  lua_pushstring(L, classic ? "TPBot Classic" : "TPBot Edu");
  lua_setfield(L, -2, "robot");
  return 1;
}

// The sonar, the same on both robots: the echo's microseconds times half
// the speed of sound. It answers in mm, the unit run_distance takes, so a
// reading can go straight into a move. Upstream answered in cm; the
// change to mm was agreed with its maintainer.
int tpbot_get_distance(lua_State *L) {
  int width = tpbot_echo_us();
  if (width < 0)
    lua_pushnil(L);
  else
    lua_pushnumber(L, (lua_Number)width * (lua_Number)0.1715);
  return 1;
}

// The magnitude of the distance or angle at arg, as the Edu takes it in
// two bytes; what they cannot carry is refused, not a number and infinity
// included. Under min gives 0, which sends nothing: a TPBot Edu sent 1 or
// 2 mm, or 1 degree, drives or spins on and never stops.
static int magnitude(lua_State *L, int arg, lua_Number min) {
  lua_Number x = fabs(luaL_checknumber(L, arg));
  luaL_argcheck(L, x < 65536, arg, "out of range");
  return x < min ? 0 : (int)x;
}

// A distance or a turn goes on after its frame is sent, and the next frame
// cuts it short: turn(3) straight(2) turned less than a quarter. So these
// wait until the Edu says it is done, as pxt-TPBot's pid_finish_delay does:
// its status query, then one byte read back, 1 once the move is over, then
// 400 ms for the robot to settle. The Classic ignores these frames, and a
// Classic that is read from holds the bus, so it is never asked: only the
// Classic answers at 240. At most ms, twice the move's time at the Edu's
// speed, as pxt-TPBot allows; past that the call returns, as theirs does.
// Nonzero when the robot stopped answering.
static int wait_done(int ms) {
  char done = 0;
  if (tpbot_i2c_write(240, "", 0) == 0)
    return 0;
  for (; 0 < ms; ms -= 10) {
    tpbot_sleep(10);
    if (to_robot("\xff\xf9\xa0\x01\x00", 5)
        || tpbot_i2c_read(32, &done, 1) != 0)
      return 1;
    if (done == 1) {
      tpbot_sleep(400);
      return 0;
    }
  }
  return 0;
}

int tpbot2_run_distance(lua_State *L) {
  int d = magnitude(L, 1, 10);
  char params[3];
  if (d == 0)
    return 0;
  params[0] = (char)(d >> 8);
  params[1] = (char)(d & 255);
  params[2] = lua_tonumber(L, 1) < 0 ? 3 : 0;
  if (to_edu(65, params, 3) || wait_done(2 * (7 * d + 400)))
    return no_answer(L);
  return 0;
}

int tpbot2_turn(lua_State *L) {
  int d = magnitude(L, 1, 5);
  char params[5];
  if (d == 0)
    return 0;
  params[0] = params[2] = (char)(d >> 8);
  params[1] = params[3] = (char)(d & 255);
  params[4] = lua_tonumber(L, 1) < 0 ? 2 : 1;
  if (to_edu(66, params, 5) || wait_done(2 * (8 * d + 500)))
    return no_answer(L);
  return 0;
}

// turn(h): h hours on a clock face, 30 degrees each, the shorter way round.
int tpbot2_turn_hours(lua_State *L) {
  lua_Number h = luaL_checknumber(L, 1);
  h = h - floor(h / 12) * 12;
  if (6 < h)
    h = h - 12;
  lua_settop(L, 0);
  lua_pushnumber(L, -30 * h);
  return tpbot2_turn(L);
}

// straight(l): l squares of 14.5 cm.
int tpbot2_straight(lua_State *L) {
  lua_Number l = luaL_checknumber(L, 1);
  lua_settop(L, 0);
  lua_pushnumber(L, 145 * l);
  return tpbot2_run_distance(L);
}

void tpbot_register_globals(lua_State *L) {
#define X(name, function) lua_register(L, #name, function);
  TPBOT_GLOBALS
#undef X
}

void tpbot2_register_globals(lua_State *L) {
#define X(name, function) lua_register(L, #name, function);
  TPBOT2_GLOBALS
#undef X
}
