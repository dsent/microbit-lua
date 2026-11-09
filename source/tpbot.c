// -*- mode: c; indent-tabs-mode: nil; -*-
//
// TPBot commands, for the TPBot Classic and the TPBot Edu
//
// Based on https://github.com/elecfreaks/pxt-TPBot (V1.ts for the Classic,
// V2.ts for the Edu). tpbot has the Classic's commands and tpbot2 the
// Edu's, each as its robot takes them: a command sends its robot's frame
// and returns. Both have info, which tells the robots apart, and the sonar.

#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "tpbot.h"

const char robot_no_answer[] =
  "The robot does not answer. Check that it is "
  "switched on, or switch it off and on again.";

// The message, said with no position in front, when the frame was not
// taken: nonzero failed.
static int sent(lua_State *L, int failed) {
  if (failed) {
    lua_pushstring(L, robot_no_answer);
    return lua_error(L);
  }
  return 0;
}

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

// The number at arg, low to high once its fraction is dropped toward 0, as
// luaL_checkint drops it. Checked as a number before it becomes an int, so
// one too large for an int, infinity or not a number is refused too.
static int whole_at(lua_State *L, int arg, int low, int high) {
  lua_Number x = luaL_checknumber(L, arg);
  luaL_argcheck(L, low - 1 < x && x < high + 1, arg, "out of range");
  return (int)x;
}

// The lights at 1 to 3, 0 to 255, into three bytes
static void lights_at(lua_State *L, char *rgb) {
  int i;
  for (i = 0; i < 3; i++)
    rgb[i] = (char)whole_at(L, i + 1, 0, 255);
}

int tpbot_set_car_light(lua_State *L) {
  char frame[4];
  frame[0] = 32;
  lights_at(L, frame + 1);
  return sent(L, to_robot(frame, 4));
}

int tpbot2_set_car_light(lua_State *L) {
  char rgb[3];
  lights_at(L, rgb);
  return sent(L, to_edu(48, rgb, 3));
}

// The speeds at 1 and 2, -255 to 255, into the two wheels' sizes. Their
// way, a backward wheel's bit set, is the robot's to add.
static void speeds_at(lua_State *L, char *sizes, int *left, int *right) {
  *left = whole_at(L, 1, -255, 255);
  *right = whole_at(L, 2, -255, 255);
  sizes[0] = (char)abs(*left);
  sizes[1] = (char)abs(*right);
}

// A TPBot Classic has been seen letting a wheel creep on after a forward
// run when stopped with its reverse bit clear; with the bit set it stays
// still, so a stopped wheel gets it.
int tpbot_set_motors_speed(lua_State *L) {
  char frame[4];
  int left, right;
  frame[0] = 1;
  speeds_at(L, frame + 1, &left, &right);
  frame[3] = (char)((left <= 0) + 2 * (right <= 0));
  return sent(L, to_robot(frame, 4));
}

int tpbot2_set_motors_speed(lua_State *L) {
  char params[3];
  int left, right;
  speeds_at(L, params, &left, &right);
  params[2] = (char)((left < 0) + 2 * (right < 0));
  return sent(L, to_edu(16, params, 3));
}

// A distance (turn 0) or a turn (turn 1) of the number at 1, mm or
// degrees, its sign the way. It goes on after its frame is sent, until
// done() says it is over, and a distance or a turn sent before then cuts
// it short. Its size must fit the Edu's two bytes: what they cannot carry
// is refused, not a number and infinity included. Under 1 does nothing,
// as upstream's did.
static int edu_move(lua_State *L, int turn) {
  lua_Number x = luaL_checknumber(L, 1);
  lua_Number size = x < 0 ? -x : x;
  char params[5];
  int d;
  luaL_argcheck(L, size < 65536, 1, "out of range");
  if (size < 1)
    return 0;
  d = (int)size;
  params[0] = params[2] = (char)(d >> 8);
  params[1] = params[3] = (char)(d & 255);
  if (turn)
    params[4] = x < 0 ? 2 : 1;
  else
    params[2] = x < 0 ? 3 : 0;
  return sent(L, to_edu(65 + turn, params, 3 + 2 * turn));
}

int tpbot2_run_distance(lua_State *L) { return edu_move(L, 0); }
int tpbot2_turn(lua_State *L) { return edu_move(L, 1); }

// The Edu's done flag, as pxt-TPBot's pid_finish_delay asks for it: its
// status query, then one byte read back, 1 once the Edu's last distance
// or turn is over. For an Edu only: a TPBot Classic that is read from
// holds the bus until it is switched off and on, so a program that may
// meet one asks info() first.
int tpbot2_done(lua_State *L) {
  char flag = 0;
  sent(L, to_robot("\xff\xf9\xa0\x01\x00", 5)
       || tpbot_i2c_read(32, &flag, 1) != 0);
  lua_pushboolean(L, flag == 1);
  return 1;
}

// Whether a robot answers a command neither one acts on: the Edu's status
// query, which the Classic ignores. Then which one it is: of the robots
// tried so far, only the Classic answers an empty write at address 240.
// An empty write alone proves nothing, since a switched-off Edu seems to
// take any.
int tpbot_info(lua_State *L) {
  int connected;
  lua_createtable(L, 0, 2);
  connected = tpbot_i2c_write(32, "\xff\xf9\xa0\x01\x00", 5) == 0;
  lua_pushboolean(L, connected);
  lua_setfield(L, -2, "connected");
  if (connected) {
    lua_pushstring(L, tpbot_i2c_write(240, "", 0) == 0
                   ? "TPBot Classic" : "TPBot Edu");
    lua_setfield(L, -2, "robot");
  }
  return 1;
}

// The sonar, the same on both robots: the echo's microseconds times half
// the speed of sound. It answers in mm, the unit run_distance takes, so a
// reading can go straight into a move. Upstream answered in cm; the
// change to mm was previously discussed with its maintainer.
int tpbot_get_distance(lua_State *L) {
  int width = tpbot_echo_us();
  if (width < 0)
    lua_pushnil(L);
  else
    lua_pushnumber(L, (lua_Number)width * (lua_Number)0.1715);
  return 1;
}
