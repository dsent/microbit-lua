// -*- mode: c; indent-tabs-mode: nil; -*-
//
// Host test for source/tpbot.c: each case runs a line of Lua over a
// stand-in bus and checks the frames written, in order, and what the call
// returned or the message it stopped on, matched at its end, after the
// position Lua puts in front of a bad argument. A frame is written as its
// bytes in hex, with its address in front when that is not the robots' 32.
//
// Usage: tpbot-host-test

#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "tpbot.h"

// What the bus does: every robot is at address 32, and only the Classic
// answers at 240, so which robot is there shows only in robot_info.
enum plan { CLASSIC, EDU, EDU_BUSY, NONE, REFUSED_FROM };

static struct {
  enum plan plan;
  int from;            // REFUSED_FROM: the first write refused, from 1
  int writes;
  int echo;            // the sonar's echo in microseconds, -1 for none
  int busy;            // how many status reads answer 0 before the 1
  char log[512];
} bus;

static void say(const char *text) {
  size_t n = strlen(bus.log);
  snprintf(bus.log + n, sizeof bus.log - n, "%s", text);
}

int tpbot_i2c_write(int address, const char *data, size_t length) {
  char text[16];
  size_t i;
  int n = ++bus.writes, took;
  switch (bus.plan) {
  case CLASSIC: took = 1; break;
  case EDU: case EDU_BUSY: took = address == 32; break;
  case REFUSED_FROM: took = n < bus.from; break;
  default: took = 0;
  }
  if (address != 32) {
    snprintf(text, sizeof text, "%d:", address);
    say(text);
  }
  for (i = 0; i < length; i++) {
    snprintf(text, sizeof text, i ? " %02X" : "%02X", (unsigned char)data[i]);
    say(text);
  }
  say(took ? "|" : " refused|");
  return took ? 0 : -1;
}

// The Edu's status: 0 while a move runs, 1 once it is over. A Classic read
// from holds the bus, so a read there shows in the frames.
int tpbot_i2c_read(int address, char *data, size_t length) {
  char text[24];
  memset(data, bus.busy > 0 ? 0 : 1, length);
  if (bus.busy > 0)
    bus.busy--;
  snprintf(text, sizeof text, "%sread %d:%02X|",
           bus.plan == CLASSIC ? "CLASSIC " : "", address,
           (unsigned char)data[0]);
  say(text);
  return bus.plan == NONE ? -1 : 0;
}

void tpbot_sleep(uint32_t ms) {
  char text[24];
  snprintf(text, sizeof text, "sleep %lu|", (unsigned long)ms);
  say(text);
}

int tpbot_echo_us(void) {
  return bus.echo;
}

// Which module's require a case runs under: each makes its own globals.
enum module { TPBOT, TPBOT2 };

#define MOVE_STOPS_BOTH "FF F9 10 03 00 00 00|01 00 00 03|"
#define MOVE_STOPS_EDU "FF F9 10 03 00 00 00|"
// A distance or a turn waits for the Edu's status to read 1, then settles
#define WAIT_DONE "240: refused|sleep 10|FF F9 A0 01 00|read 32:01|sleep 400|"

static const struct test {
  enum module module;
  const char *code;
  enum plan plan;
  const char *frames;
  const char *result;  // what it returned, or the message it stopped on
} tests[] = {
  // tpbot, either robot. robot_info: the Edu's status query, then the
  // Classic's address
  {TPBOT, "return robot_info()", NONE, "FF F9 A0 01 00 refused|",
   "connected=false"},
  {TPBOT, "return robot_info()", EDU, "FF F9 A0 01 00|240: refused|",
   "connected=true robot=TPBot Edu"},
  {TPBOT, "return robot_info()", CLASSIC, "FF F9 A0 01 00|240:|",
   "connected=true robot=TPBot Classic"},

  // robot_move: both robots' frames, the sleep, both stops
  {TPBOT, "robot_move(50, 50, 1)", CLASSIC,
   "FF F9 10 03 32 32 00|01 32 32 00|sleep 1000|" MOVE_STOPS_BOTH, ""},
  {TPBOT, "robot_move(-40, 40, 0.7)", EDU,
   "FF F9 10 03 28 28 01|01 28 28 01|sleep 700|" MOVE_STOPS_BOTH, ""},
  // a stopped wheel gets the Classic's reverse bit
  {TPBOT, "robot_move(0, 100, 2)", CLASSIC,
   "FF F9 10 03 00 64 00|01 00 64 01|sleep 2000|" MOVE_STOPS_BOTH, ""},
  {TPBOT, "robot_move(255, -255, 0)", CLASSIC,
   "FF F9 10 03 FF FF 02|01 FF FF 02|sleep 0|" MOVE_STOPS_BOTH, ""},
  {TPBOT, "robot_move(256, 0, 1)", CLASSIC, "",
   "bad argument #1 to 'robot_move' (out of range)"},
  {TPBOT, "robot_move(50, -300, 1)", CLASSIC, "",
   "bad argument #2 to 'robot_move' (out of range)"},
  {TPBOT, "robot_move('fast', 50, 1)", CLASSIC, "",
   "bad argument #1 to 'robot_move' (number expected, got string)"},
  {TPBOT, "robot_move(50, 50)", CLASSIC, "",
   "robot_move needs how many seconds to drive, from 0 to 3600, "
   "such as robot_move(50, 50, 1)."},
  {TPBOT, "robot_move(50, 50, -1)", CLASSIC, "",
   "robot_move needs how many seconds to drive, from 0 to 3600, "
   "such as robot_move(50, 50, 1)."},
  // no robot: nothing moved, so no stop is sent
  {TPBOT, "robot_move(50, 50, 1)", NONE, "FF F9 10 03 32 32 00 refused|",
   tpbot_no_answer},
  // the robot lost after the first frame: both stops still go out
  {TPBOT, "robot_move(50, 50, 1)", REFUSED_FROM,
   "FF F9 10 03 32 32 00|01 32 32 00 refused|"
   "FF F9 10 03 00 00 00 refused|01 00 00 03 refused|", tpbot_no_answer},

  // tpbot's own commands
  {TPBOT, "tpbot.set_motors_speed(30, -30)", CLASSIC,
   "FF F9 10 03 1E 1E 02|01 1E 1E 02|", ""},
  {TPBOT, "tpbot.set_car_light(255, 0, 128)", CLASSIC,
   "FF F9 30 03 FF 00 80|20 FF 00 80|", ""},
  {TPBOT, "tpbot.set_car_light(256, 0, 0)", CLASSIC, "",
   "bad argument #1 to 'set_car_light' (out of range)"},
  {TPBOT, "tpbot.set_car_light(0, 0, 0)", NONE,
   "FF F9 30 03 00 00 00 refused|", tpbot_no_answer},
  {TPBOT, "return tpbot.get_distance()", CLASSIC, "", "171.5"},

  // the Edu's commands are not tpbot's: Lua says so, and nothing goes out
  {TPBOT, "straight(1)", EDU, "",
   "attempt to call global 'straight' (a nil value)"},
  {TPBOT, "turn(3)", EDU, "", "attempt to call global 'turn' (a nil value)"},
  {TPBOT, "tpbot.run_distance(100)", EDU, "",
   "attempt to call field 'run_distance' (a nil value)"},

  // tpbot2, the Edu: its frames only
  {TPBOT2, "robot_move(50, -50, 1)", EDU,
   "FF F9 10 03 32 32 02|sleep 1000|" MOVE_STOPS_EDU, ""},
  {TPBOT2, "robot_move(50, 50, 1)", NONE, "FF F9 10 03 32 32 00 refused|",
   tpbot_no_answer},
  {TPBOT2, "return robot_info()", EDU, "FF F9 A0 01 00|240: refused|",
   "connected=true robot=TPBot Edu"},
  {TPBOT2, "tpbot2.set_motors_speed(30, -30)", EDU, "FF F9 10 03 1E 1E 02|",
   ""},
  {TPBOT2, "tpbot2.set_car_light(255, 0, 128)", EDU, "FF F9 30 03 FF 00 80|",
   ""},
  {TPBOT2, "return tpbot2.get_distance()", EDU, "", "171.5"},

  // straight(l): l squares of 145 mm
  {TPBOT2, "straight(1)", EDU,
   "FF F9 41 03 00 91 00|" WAIT_DONE, ""},
  {TPBOT2, "straight(-2)", EDU,
   "FF F9 41 03 01 22 03|" WAIT_DONE, ""},
  {TPBOT2, "straight(0.05)", EDU, "", ""},
  {TPBOT2, "straight(0)", EDU, "", ""},
  {TPBOT2, "straight(1/0)", EDU, "",
   "bad argument #1 to 'straight' (out of range)"},
  {TPBOT2, "straight(1)", NONE, "FF F9 41 03 00 91 00 refused|",
   tpbot_no_answer},

  // the Classic ignores the frame and is never read from
  {TPBOT2, "straight(1)", CLASSIC, "FF F9 41 03 00 91 00|240:|", ""},
  // a move still running: the status is asked again every 10 ms
  {TPBOT2, "straight(1)", EDU_BUSY,
   "FF F9 41 03 00 91 00|240: refused|sleep 10|FF F9 A0 01 00|read 32:00|"
   "sleep 10|FF F9 A0 01 00|read 32:00|sleep 10|FF F9 A0 01 00|read 32:01|"
   "sleep 400|", ""},
  // the robot lost while the move runs
  {TPBOT2, "turn(3)", REFUSED_FROM,
   "FF F9 42 05 00 5A 00 5A 02|240: refused|sleep 10|"
   "FF F9 A0 01 00 refused|", tpbot_no_answer},

  // turn(h): h hours of the clock, the short way round
  {TPBOT2, "turn(3)", EDU,
   "FF F9 42 05 00 5A 00 5A 02|" WAIT_DONE, ""},
  {TPBOT2, "turn(9)", EDU,
   "FF F9 42 05 00 5A 00 5A 01|" WAIT_DONE, ""},
  {TPBOT2, "turn(6)", EDU,
   "FF F9 42 05 00 B4 00 B4 02|" WAIT_DONE, ""},
  {TPBOT2, "turn(0.1)", EDU, "", ""},
  {TPBOT2, "turn('left')", EDU, "",
   "bad argument #1 to 'turn' (number expected, got string)"},

  // the dead band and the two bytes' range
  {TPBOT2, "tpbot2.run_distance(10)", EDU,
   "FF F9 41 03 00 0A 00|" WAIT_DONE, ""},
  {TPBOT2, "tpbot2.run_distance(9)", EDU, "", ""},
  {TPBOT2, "tpbot2.run_distance(65535)", EDU,
   "FF F9 41 03 FF FF 00|" WAIT_DONE, ""},
  {TPBOT2, "tpbot2.run_distance(65536)", EDU, "",
   "bad argument #1 to 'run_distance' (out of range)"},
  {TPBOT2, "tpbot2.turn(-5)", EDU,
   "FF F9 42 05 00 05 00 05 02|" WAIT_DONE, ""},
  {TPBOT2, "tpbot2.turn(4)", EDU, "", ""},
};

// What the call left on the stack, as the tests write it
static void result_of(lua_State *L, char *out, size_t size) {
  if (lua_gettop(L) == 0) {
    snprintf(out, size, "%s", "");
  } else if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "connected");
    lua_getfield(L, 1, "robot");
    snprintf(out, size, "connected=%s%s%s",
             lua_toboolean(L, -2) ? "true" : "false",
             lua_isstring(L, -1) ? " robot=" : "",
             lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
  } else if (lua_isnil(L, 1)) {
    snprintf(out, size, "nil");
  } else {
    snprintf(out, size, "%.4g", (double)lua_tonumber(L, 1));
  }
}

static int run(lua_State *L, const struct test *t, int echo) {
  char result[256];
  size_t at;
  memset(&bus, 0, sizeof bus);
  bus.plan = t->plan;
  bus.from = 2;
  bus.busy = t->plan == EDU_BUSY ? 2 : 0;
  bus.echo = echo;
  lua_settop(L, 0);
  if (luaL_loadstring(L, t->code) != 0
      || lua_pcall(L, 0, LUA_MULTRET, 0) != 0)
    snprintf(result, sizeof result, "%s", lua_tostring(L, -1));
  else
    result_of(L, result, sizeof result);
  at = strlen(result) - strlen(t->result);
  if (strcmp(bus.log, t->frames) == 0 && strlen(result) >= strlen(t->result)
      && strcmp(result + at, t->result) == 0
      && (at == 0 || strncmp(result + at - 2, ": ", 2) == 0))
    return 0;
  printf("FAIL %s\n  frames   %s\n  expected %s\n  result   %s\n"
         "  expected %s\n", t->code, bus.log, t->frames, result, t->result);
  return 1;
}

// A Lua state with the module as the firmware's require makes it: its
// table, then the globals its open hook sets.
static lua_State *state(enum module module) {
  lua_State *L = luaL_newstate();
  luaopen_base(L);
  lua_newtable(L);
#define X(name, function)                       \
  lua_pushcfunction(L, function);               \
  lua_setfield(L, -2, #name);
  if (module == TPBOT) {
    TPBOT_FUNCTIONS
    lua_setglobal(L, "tpbot");
    tpbot_register_globals(L);
  } else {
    TPBOT2_FUNCTIONS
    lua_setglobal(L, "tpbot2");
    tpbot2_register_globals(L);
  }
#undef X
  return L;
}

int main(void) {
  static const struct test no_echo =
    {TPBOT, "return tpbot.get_distance()", CLASSIC, "", "nil"};
  lua_State *states[2];
  size_t i, n = sizeof tests / sizeof tests[0];
  int failed = 0;
  states[TPBOT] = state(TPBOT);
  states[TPBOT2] = state(TPBOT2);
  for (i = 0; i < n; i++)
    failed += run(states[tests[i].module], &tests[i], 1000);
  failed += run(states[TPBOT], &no_echo, -1);
  printf("tpbot: %d of %d cases failed\n", failed, (int)n + 1);
  lua_close(states[TPBOT]);
  lua_close(states[TPBOT2]);
  return failed != 0;
}
