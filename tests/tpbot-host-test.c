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
// answers at 240. EDU_BUSY's done flag reads 0, EDU_SILENT takes writes
// but answers no read.
enum plan { CLASSIC, EDU, EDU_BUSY, EDU_SILENT, NONE };

static struct {
  enum plan plan;
  int echo;            // the sonar's echo in microseconds, -1 for none
  char log[512];
} bus;

static void say(const char *text) {
  size_t n = strlen(bus.log);
  snprintf(bus.log + n, sizeof bus.log - n, "%s", text);
}

int tpbot_i2c_write(int address, const char *data, size_t length) {
  char text[16];
  size_t i;
  int took = bus.plan == CLASSIC || (bus.plan != NONE && address == 32);
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

// The Edu's done flag. A real Classic read from holds the bus, so a read
// there shows in the frames.
int tpbot_i2c_read(int address, char *data, size_t length) {
  char text[24];
  int answered = bus.plan != EDU_SILENT && bus.plan != NONE;
  memset(data, bus.plan == EDU_BUSY ? 0 : 1, length);
  snprintf(text, sizeof text, "%sread %d:%s|",
           bus.plan == CLASSIC ? "CLASSIC " : "", address,
           !answered ? "none" : data[0] ? "01" : "00");
  say(text);
  return answered ? 0 : -1;
}

int tpbot_echo_us(void) {
  return bus.echo;
}

// Which module's require a case runs under
enum module { TPBOT, TPBOT2 };

#define STATUS "FF F9 A0 01 00"

static const struct test {
  enum module module;
  const char *code;
  enum plan plan;
  const char *frames;
  const char *result;  // what it returned, or the message it stopped on
} tests[] = {
  // tpbot, the Classic. info: the Edu's status query, which the Classic
  // ignores, then the Classic's address
  {TPBOT, "return tpbot.info()", NONE, STATUS " refused|", "connected=false"},
  {TPBOT, "return tpbot.info()", EDU, STATUS "|240: refused|",
   "connected=true robot=TPBot Edu"},
  {TPBOT, "return tpbot.info()", CLASSIC, STATUS "|240:|",
   "connected=true robot=TPBot Classic"},

  {TPBOT, "tpbot.set_car_light(255, 0, 128)", CLASSIC, "20 FF 00 80|", ""},
  {TPBOT, "tpbot.set_car_light(256, 0, 0)", CLASSIC, "",
   "bad argument #1 to 'set_car_light' (out of range)"},
  {TPBOT, "tpbot.set_car_light(0, 1e10, 0)", CLASSIC, "",
   "bad argument #2 to 'set_car_light' (out of range)"},
  {TPBOT, "tpbot.set_car_light(0, 0, -1)", CLASSIC, "",
   "bad argument #3 to 'set_car_light' (out of range)"},
  {TPBOT, "tpbot.set_car_light(0, 0, 0)", NONE, "20 00 00 00 refused|",
   robot_no_answer},

  {TPBOT, "tpbot.set_motors_speed(30, -30)", CLASSIC, "01 1E 1E 02|", ""},
  // a stopped wheel gets its reverse bit
  {TPBOT, "tpbot.set_motors_speed(0, 100)", CLASSIC, "01 00 64 01|", ""},
  {TPBOT, "tpbot.set_motors_speed(0, 0)", CLASSIC, "01 00 00 03|", ""},
  {TPBOT, "tpbot.set_motors_speed(255, -255)", CLASSIC, "01 FF FF 02|", ""},
  // a fraction is dropped toward 0, as luaL_checkint drops it
  {TPBOT, "tpbot.set_motors_speed(30.7, -30.7)", CLASSIC, "01 1E 1E 02|",
   ""},
  {TPBOT, "tpbot.set_motors_speed(256, 0)", CLASSIC, "",
   "bad argument #1 to 'set_motors_speed' (out of range)"},
  {TPBOT, "tpbot.set_motors_speed(50, -300)", CLASSIC, "",
   "bad argument #2 to 'set_motors_speed' (out of range)"},
  {TPBOT, "tpbot.set_motors_speed('fast', 50)", CLASSIC, "",
   "bad argument #1 to 'set_motors_speed' (number expected, got string)"},
  // checked as a number before it becomes an int
  {TPBOT, "tpbot.set_motors_speed(-2147483648, 0)", CLASSIC, "",
   "bad argument #1 to 'set_motors_speed' (out of range)"},
  {TPBOT, "tpbot.set_motors_speed(0, 0/0)", CLASSIC, "",
   "bad argument #2 to 'set_motors_speed' (out of range)"},
  {TPBOT, "tpbot.set_motors_speed(30, 30)", NONE, "01 1E 1E 00 refused|",
   robot_no_answer},

  {TPBOT, "return tpbot.get_distance()", CLASSIC, "", "171.5"},
  // the Edu's commands are tpbot2's: Lua says so, and nothing goes out
  {TPBOT, "tpbot.run_distance(145)", CLASSIC, "",
   "attempt to call field 'run_distance' (a nil value)"},

  // tpbot2, the Edu: one Edu frame each, and info as tpbot's
  {TPBOT2, "return tpbot2.info()", EDU, STATUS "|240: refused|",
   "connected=true robot=TPBot Edu"},
  {TPBOT2, "tpbot2.set_car_light(255, 0, 128)", EDU,
   "FF F9 30 03 FF 00 80|", ""},
  {TPBOT2, "tpbot2.set_car_light(0, 0, 256)", EDU, "",
   "bad argument #3 to 'set_car_light' (out of range)"},
  {TPBOT2, "tpbot2.set_motors_speed(30, -30)", EDU,
   "FF F9 10 03 1E 1E 02|", ""},
  // the Edu's stopped wheel keeps its bit clear
  {TPBOT2, "tpbot2.set_motors_speed(0, 100)", EDU,
   "FF F9 10 03 00 64 00|", ""},
  {TPBOT2, "tpbot2.set_motors_speed(256, 0)", EDU, "",
   "bad argument #1 to 'set_motors_speed' (out of range)"},
  {TPBOT2, "tpbot2.set_motors_speed(30, 30)", NONE,
   "FF F9 10 03 1E 1E 00 refused|", robot_no_answer},
  {TPBOT2, "return tpbot2.get_distance()", EDU, "", "171.5"},

  // a distance or a turn returns once its frame is sent, under 1 does
  // nothing, and the size must fit two bytes
  {TPBOT2, "tpbot2.run_distance(145)", EDU, "FF F9 41 03 00 91 00|", ""},
  {TPBOT2, "tpbot2.run_distance(-290)", EDU, "FF F9 41 03 01 22 03|", ""},
  {TPBOT2, "tpbot2.run_distance(1)", EDU, "FF F9 41 03 00 01 00|", ""},
  {TPBOT2, "tpbot2.run_distance(0)", EDU, "", ""},
  {TPBOT2, "tpbot2.run_distance(-0.5)", EDU, "", ""},
  {TPBOT2, "tpbot2.run_distance(65535)", EDU, "FF F9 41 03 FF FF 00|", ""},
  {TPBOT2, "tpbot2.run_distance(65536)", EDU, "",
   "bad argument #1 to 'run_distance' (out of range)"},
  {TPBOT2, "tpbot2.run_distance(0/0)", EDU, "",
   "bad argument #1 to 'run_distance' (out of range)"},
  {TPBOT2, "tpbot2.run_distance(1/0)", EDU, "",
   "bad argument #1 to 'run_distance' (out of range)"},
  {TPBOT2, "tpbot2.run_distance(145)", NONE, "FF F9 41 03 00 91 00 refused|",
   robot_no_answer},
  // a turn: degrees, to the left for a positive angle
  {TPBOT2, "tpbot2.turn(1)", EDU, "FF F9 42 05 00 01 00 01 01|", ""},
  {TPBOT2, "tpbot2.turn(90)", EDU, "FF F9 42 05 00 5A 00 5A 01|", ""},
  {TPBOT2, "tpbot2.turn(-90)", EDU, "FF F9 42 05 00 5A 00 5A 02|", ""},
  {TPBOT2, "tpbot2.turn('left')", EDU, "",
   "bad argument #1 to 'turn' (number expected, got string)"},

  // done: the status query, then one byte read back
  {TPBOT2, "return tpbot2.done()", EDU, STATUS "|read 32:01|", "true"},
  {TPBOT2, "return tpbot2.done()", EDU_BUSY, STATUS "|read 32:00|", "false"},
  {TPBOT2, "return tpbot2.done()", EDU_SILENT, STATUS "|read 32:none|",
   robot_no_answer},
  {TPBOT2, "return tpbot2.done()", NONE, STATUS " refused|", robot_no_answer},
  // a plain read, whatever robot is there: a real Classic then holds the
  // bus, so a program that may meet one asks info() first
  {TPBOT2, "return tpbot2.done()", CLASSIC, STATUS "|CLASSIC read 32:01|",
   "true"},
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
  } else if (lua_isboolean(L, 1)) {
    snprintf(out, size, "%s", lua_toboolean(L, 1) ? "true" : "false");
  } else {
    snprintf(out, size, "%.4g", (double)lua_tonumber(L, 1));
  }
}

static int run(lua_State *L, const struct test *t, int echo) {
  char result[256];
  size_t at;
  memset(&bus, 0, sizeof bus);
  bus.plan = t->plan;
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
      && (at == 0 || (at >= 2 && strncmp(result + at - 2, ": ", 2) == 0)))
    return 0;
  printf("FAIL %s\n  frames   %s\n  expected %s\n  result   %s\n"
         "  expected %s\n", t->code, bus.log, t->frames, result, t->result);
  return 1;
}

// A Lua state with the module's table, as the firmware's require makes it
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
  } else {
    TPBOT2_FUNCTIONS
    lua_setglobal(L, "tpbot2");
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
