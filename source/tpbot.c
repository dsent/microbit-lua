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
// degrees, its sign the way. It goes on after its frame is sent, as long
// as busy() says it runs, and a distance or a turn sent meanwhile cuts it
// short. Its size must fit the Edu's two bytes: what they cannot carry
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

// A number in two bytes, high first
static void two_bytes(char *at, int value) {
  at[0] = (char)(value >> 8);
  at[1] = (char)(value & 255);
}

// A turn about one wheel, as pxt-TPBot's left and right steering: the
// command of turn, with the outer wheel given twice the angle and the
// inner one nothing, the right wheel for a positive angle, which turns
// left. Twice the size must fit two bytes; under 1 does nothing.
int tpbot2_pivot(lua_State *L) {
  lua_Number x = luaL_checknumber(L, 1);
  lua_Number size = x < 0 ? -x : x;
  char params[5] = { 0, 0, 0, 0, 0 };
  luaL_argcheck(L, size < 32768, 1, "out of range");
  if (size < 1)
    return 0;
  two_bytes(params + (x < 0 ? 0 : 2), 2 * (int)size);
  return sent(L, to_edu(66, params, 5));
}

// The wheels at the speeds at 1 and 2 in mm/s, by the Edu's own speed
// control, pxt-TPBot's pidSpeedControl; a negative speed is backward.
int tpbot2_speed_control(lua_State *L) {
  char params[5];
  int left = whole_at(L, 1, -65535, 65535);
  int right = whole_at(L, 2, -65535, 65535);
  two_bytes(params, abs(left));
  two_bytes(params + 2, abs(right));
  params[4] = (char)((left < 0) + 2 * (right < 0));
  return sent(L, to_edu(64, params, 5));
}

// The speed of the Edu's distances and turns in mm/s, pxt-TPBot's
// pidSetSpeed.
int tpbot2_set_speed(lua_State *L) {
  char params[2];
  two_bytes(params, whole_at(L, 1, 0, 65535));
  return sent(L, to_edu(79, params, 2));
}

// The servo at 1, S1 to S4 as 1 to 4, to the value at 2. pxt-TPBot's
// setServo and setServo360 send 0 to 180: a 180 degree servo's angle, half
// a 360 degree servo's angle, or a continuous servo's speed, 90 for still.
int tpbot2_set_servo(lua_State *L) {
  char params[2];
  params[0] = (char)whole_at(L, 1, 0, 255);
  params[1] = (char)whole_at(L, 2, 0, 255);
  return sent(L, to_edu(32, params, 2));
}

// pxt-TPBot's adjustMotorSpeed and resetMotorAdjust, as they send them:
// one parameter, 0, which "" holds
int tpbot2_adjust_motor_speed(lua_State *L) {
  return sent(L, to_edu(240, "", 1));
}

int tpbot2_reset_motor_adjust(lua_State *L) {
  return sent(L, to_edu(241, "", 1));
}

// Whether the Edu's last distance or turn still runs, from its done flag,
// as pxt-TPBot's pid_finish_delay asks for it: its status query, then one
// byte read back, 1 once the move is over. For an Edu only: a TPBot
// Classic that is read from holds the bus until it is switched off and
// on, so a program that may meet one asks info() first.
int tpbot2_busy(lua_State *L) {
  char flag = 0;
  sent(L, to_robot("\xff\xf9\xa0\x01\x00", 5)
       || tpbot_i2c_read(32, &flag, 1) != 0);
  lua_pushboolean(L, flag != 1);
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
