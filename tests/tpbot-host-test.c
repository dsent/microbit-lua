// -*- mode: c; indent-tabs-mode: nil; -*-
//
// Host test for source/tpbot.c: runs the TPBot commands as the Lua had
// them (tests/tpbot-reference.lua, loaded and stripped as the firmware
// loads its script) beside the C, in this firmware's Lua built for the
// host, and asks each the same calls under the same bus. Both must write
// the same bytes to the same addresses, sleep as long, return the same
// values and stop on the same message. The one difference let through:
// the position the Lua put in front of a message it raised itself
// ("[string "embedded"]:0: "), which the C has no line to give.
//
// Usage: tpbot-host-test tests/tpbot-reference.lua
// With TPBOT_TEST_SHOW set in the environment it prints every case.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "tpbot.h"

void lua_strip_debug(lua_State *L);

// What the bus does with each write in a case.
enum plan { ALL_TAKEN, NONE_TAKEN, ONE_REFUSED, REFUSED_FROM, EDU_ONLY };

static struct {
  enum plan plan;
  int at;              // the write ONE_REFUSED and REFUSED_FROM count to
  int writes;          // writes so far in this case
  int echo;            // what the sonar gives, -1 for none
  char log[8192];      // what happened, in order
} bus;

static void logf_(const char *text) {
  size_t n = strlen(bus.log);
  snprintf(bus.log + n, sizeof bus.log - n, "%s", text);
}

static int bus_takes(int address) {
  int n = ++bus.writes;
  switch (bus.plan) {
  case ALL_TAKEN: return 1;
  case NONE_TAKEN: return 0;
  case ONE_REFUSED: return n != bus.at;
  case REFUSED_FROM: return n < bus.at;
  case EDU_ONLY: return address == 32;
  }
  return 0;
}

// The board, for the C.

int tpbot_i2c_write(int address, const char *data, size_t length) {
  char text[64];
  int took = bus_takes(address);
  size_t i;
  snprintf(text, sizeof text, "write %d [", address);
  logf_(text);
  for (i = 0; i < length; i++) {
    snprintf(text, sizeof text, i ? " %02X" : "%02X", (unsigned char)data[i]);
    logf_(text);
  }
  logf_(took ? "] taken\n" : "] refused\n");
  return took ? 0 : -1;
}

void tpbot_sleep(uint32_t ms) {
  char text[32];
  snprintf(text, sizeof text, "sleep %lu\n", (unsigned long)ms);
  logf_(text);
}

int tpbot_echo_us(void) {
  logf_("sonar\n");
  return bus.echo;
}

// The board, for the Lua: microbit.i2c.write, microbit.sleep and the pin
// calls as the firmware binds them, over the same bus.

static int l_i2c_write(lua_State *L) {
  int address = luaL_checkint(L, 1);
  size_t length;
  const char *data = luaL_checklstring(L, 2, &length);
  if (tpbot_i2c_write(address, data, length) == 0)
    return 0;
  return luaL_error(L, "i2c write error");
}

static int l_sleep(lua_State *L) {
  uint32_t ms = (uint32_t)luaL_checkinteger(L, 1);
  tpbot_sleep(ms);
  return 0;
}

static int l_get_pin(lua_State *L) {
  lua_pushlightuserdata(L, (void *)(intptr_t)luaL_checkint(L, 1));
  return 1;
}

static int l_nothing(lua_State *L) {
  (void)L;
  return 0;
}

// getPulseUs: the echo's width, or nil
static int l_get_pulse_us(lua_State *L) {
  int width = tpbot_echo_us();
  if (width < 0)
    lua_pushnil(L);
  else
    lua_pushinteger(L, width);
  return 1;
}

static void board(lua_State *L) {
  lua_newtable(L);
  lua_pushcfunction(L, l_sleep);
  lua_setfield(L, -2, "sleep");
  lua_newtable(L);
  lua_pushcfunction(L, l_i2c_write);
  lua_setfield(L, -2, "write");
  lua_setfield(L, -2, "i2c");
  lua_newtable(L);
  lua_pushcfunction(L, l_get_pin);
  lua_setfield(L, -2, "getPin");
  lua_pushcfunction(L, l_nothing);
  lua_setfield(L, -2, "getDigitalValue");
  lua_pushcfunction(L, l_nothing);
  lua_setfield(L, -2, "pulseUs");
  lua_pushcfunction(L, l_get_pulse_us);
  lua_setfield(L, -2, "getPulseUs");
  lua_setfield(L, -2, "io");
  lua_setglobal(L, "microbit");
}

// A value as the REPL shows it, in the order pairs() gives.
static const char *SHOW =
  "local function show(v, seen)\n"
  "  if type(v) == 'string' then return string.format('%q', v) end\n"
  "  if type(v) ~= 'table' then return type(v) .. ' ' .. tostring(v) end\n"
  "  local out = {}\n"
  "  for k, x in pairs(v) do out[#out + 1] = show(k) .. '=' .. show(x) end\n"
  "  return '{' .. table.concat(out, ', ') .. '}'\n"
  "end\n"
  "return function(code)\n"
  "  local f, err = loadstring(code, 'REPL')\n"
  "  if not f then return 'compile ' .. err end\n"
  "  local r = (function(...) return { n = select('#', ...), ... } end)(pcall(f))\n"
  "  if not r[1] then return 'error ' .. tostring(r[2]) end\n"
  "  local out = {}\n"
  "  for i = 2, r.n do out[#out + 1] = show(r[i]) end\n"
  "  return 'returned ' .. table.concat(out, ', ')\n"
  "end\n";

static const char *reference_text;
static size_t reference_length;

static lua_State *state(int lua) {
  lua_State *L = luaL_newstate();
  luaopen_base(L);
  luaopen_table(L);
  luaopen_string(L);
  luaopen_math(L);
  lua_settop(L, 0);
  board(L);
  if (lua) {
    if (luaL_loadbuffer(L, reference_text, reference_length, "embedded")) {
      fprintf(stderr, "%s\n", lua_tostring(L, -1));
      exit(2);
    }
    lua_strip_debug(L);
    if (lua_pcall(L, 0, 0, 0)) {
      fprintf(stderr, "%s\n", lua_tostring(L, -1));
      exit(2);
    }
  } else {
    lua_newtable(L);
#define X(name, function) \
    lua_pushcfunction(L, function); lua_setfield(L, -2, #name);
    TPBOT_FUNCTIONS
#undef X
    lua_setglobal(L, "tpbot");
    tpbot_register_globals(L);
  }
  if (luaL_loadstring(L, SHOW) || lua_pcall(L, 0, 1, 0)) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    exit(2);
  }
  lua_setglobal(L, "run_case");
  return L;
}

static char heard[2][16384];
static int positions;

// Run code in a fresh state: what the bus saw, then what came back.
static void run(int lua, const char *code, char *out, size_t size) {
  lua_State *L = state(lua);
  const char *said;
  static const char POSITION[] = "error [string \"embedded\"]:0: ";
  bus.writes = 0;
  bus.log[0] = 0;
  lua_getglobal(L, "run_case");
  lua_pushstring(L, code);
  lua_call(L, 1, 1);
  said = lua_tostring(L, -1);
  if (lua && strncmp(said, POSITION, sizeof POSITION - 1) == 0) {
    positions++;
    snprintf(out, size, "%serror %s", bus.log, said + sizeof POSITION - 1);
  } else {
    snprintf(out, size, "%s%s", bus.log, said);
  }
  lua_close(L);
}

static const char *CALLS[] = {
  // lights
  "tpbot.set_car_light(0, 0, 0)", "tpbot.set_car_light(255, 255, 255)",
  "tpbot.set_car_light(1, 2, 3)", "tpbot.set_car_light(256, 0, 0)",
  "tpbot.set_car_light(0, -1, 0)", "tpbot.set_car_light(0, 0, 1.9)",
  "tpbot.set_car_light(-0.5, 255.99, 0)", "tpbot.set_car_light('10', 0, 0)",
  "tpbot.set_car_light('x', 0, 0)", "tpbot.set_car_light(1, 2)",
  "tpbot.set_car_light()", "tpbot.set_car_light({}, 0, 0)",
  "tpbot.set_car_light(true, 0, 0)", "tpbot.set_car_light(1e9, 0, 0)",
  "tpbot.set_car_light(0, 0, -1e9)", "tpbot.set_car_light(1, 2, 3, 4)",
  // motors
  "tpbot.set_motors_speed(0, 0)", "tpbot.set_motors_speed(50, 50)",
  "tpbot.set_motors_speed(-50, 50)", "tpbot.set_motors_speed(50, -50)",
  "tpbot.set_motors_speed(-50, -50)", "tpbot.set_motors_speed(100, 100)",
  "tpbot.set_motors_speed(255, -255)", "tpbot.set_motors_speed(256, 0)",
  "tpbot.set_motors_speed(0, -256)", "tpbot.set_motors_speed(0.5, 0.5)",
  "tpbot.set_motors_speed(-0.5, -0.99)", "tpbot.set_motors_speed(0.99, 1)",
  "tpbot.set_motors_speed(-0, 0)", "tpbot.set_motors_speed(-1, 1.5)",
  "tpbot.set_motors_speed(1, 1)", "tpbot.set_motors_speed(1, -1)",
  "tpbot.set_motors_speed(-1, 0)", "robot_move(1, 1, 1)",
  "tpbot.set_motors_speed('5', 0)", "tpbot.set_motors_speed(0, '5')",
  "tpbot.set_motors_speed('x', 0)", "tpbot.set_motors_speed(nil, 0)",
  "tpbot.set_motors_speed(0)", "tpbot.set_motors_speed()",
  "tpbot.set_motors_speed({}, 0)", "tpbot.set_motors_speed(0, true)",
  "tpbot.set_motors_speed(1e9, 0)", "tpbot.set_motors_speed(-1e9, 0)",
  // robot_move
  "robot_move(50, 50, 1)", "robot_move(50, 50, 0)", "robot_move(50, 50, 3600)",
  "robot_move(-50, 50, 2)", "robot_move(0, 0, 1)", "robot_move(50, 50, 1.5)",
  "robot_move(50, 50, 0.0005)", "robot_move(50, 50, 0.0015)",
  "robot_move(50, 50, 3600.5)", "robot_move(50, 50, -1)",
  "robot_move(50, 50, -0.0001)", "robot_move(50, 50, '1')",
  "robot_move(50, 50, nil)", "robot_move(50, 50)", "robot_move()",
  "robot_move(50, 50, 0/0)", "robot_move(50, 50, 1/0)",
  "robot_move(50, 50, -1/0)", "robot_move(50, 50, {})",
  "robot_move(nil, 50, 1)", "robot_move('x', 50, 1)",
  "robot_move(300, 0, 1)", "robot_move(0, -300, 1)", "robot_move('5', 0, 1)",
  "robot_move(50, '5', 1)", "robot_move(255, 255, 3600)",
  "robot_move(0.5, -0.5, 1)",
  // robot_info
  "return robot_info()", "return robot_info(1, 2)",
  "local i = robot_info() return i.connected, i.robot",
  // turn and straight
  "turn(0)", "turn(1)", "turn(3)", "turn(6)", "turn(7)", "turn(11)",
  "turn(12)", "turn(-1)", "turn(-6)", "turn(1.5)", "turn(13)", "turn(-13)",
  "turn(0.01)", "turn('3')", "turn('x')", "turn(nil)", "turn()", "turn({})",
  "turn(true)", "turn(1e6)", "turn(-1e6 - 0.5)", "turn(1, 2)",
  "straight(0)", "straight(1)", "straight(-1)", "straight(2)",
  "straight(0.5)", "straight(5.97)", "straight(595)", "straight(596)",
  "straight(-595)", "straight(-596)", "straight(0.001)", "straight('2')",
  "straight('x')", "straight(nil)", "straight()", "straight({})",
  "straight(1e9)",
  "tpbot.run_distance(0)", "tpbot.run_distance(100)",
  "tpbot.run_distance(-100)", "tpbot.run_distance(255)",
  "tpbot.run_distance(256)", "tpbot.run_distance(65535)",
  "tpbot.run_distance(65536)", "tpbot.run_distance(-65536)",
  "tpbot.run_distance(0.5)", "tpbot.run_distance(-0)",
  "tpbot.run_distance(300.75)", "tpbot.run_distance('100')",
  "tpbot.run_distance('0')", "tpbot.run_distance('x')",
  "tpbot.run_distance(nil)", "tpbot.run_distance()",
  "tpbot.run_distance({})", "tpbot.run_distance(1e9)",
  "tpbot.turn(0)", "tpbot.turn(90)", "tpbot.turn(-90)", "tpbot.turn(360)",
  "tpbot.turn(180.5)", "tpbot.turn(-0.5)", "tpbot.turn(65535)",
  "tpbot.turn(65536)", "tpbot.turn('90')", "tpbot.turn('x')",
  "tpbot.turn(nil)", "tpbot.turn()", "tpbot.turn(false)",
  // turn and straight look tpbot's commands up when called
  "local got tpbot.turn = function(d) got = d end turn(3) return got",
  "local got tpbot.run_distance = function(d) got = d end straight(2) "
  "return got",
  "tpbot.turn = function() error('mine') end turn(3)",
  "tpbot.turn = nil turn(3)", "tpbot.turn = 5 turn(3)",
  "tpbot.run_distance = false straight(1)", "tpbot = nil turn(3)",
  "tpbot = nil straight(1)", "tpbot = nil straight('x')",
  "tpbot = 7 turn(1)", "tpbot = 'x' turn(1)",
  "tpbot = setmetatable({}, { __index = function(t, k) return "
  "function(d) error(k .. d) end end }) turn(1)",
  "tpbot.turn = setmetatable({}, { __call = function(self, d) "
  "error('called ' .. d) end }) turn(2)",
  "turn = nil return pcall(straight, 1)",
  // a mistake in the call itself, then one after a command
  "robot_move(50, 50, 1) turn(3) return 1",
  "tpbot.set_motors_speed(50, 50) robot_move(10, 10, 1)",
  "local ok = pcall(robot_move, 'x', 0, 1) return ok, robot_info()",
  NULL
};

static const char *SONAR[] = {
  "return tpbot.get_distance()", "return tpbot.get_distance(1, 2)",
  "return (tpbot.get_distance())", "return type(tpbot.get_distance())",
  NULL
};

static const int ECHOES[] = { -1, 0, 1, 58, 583, 1000, 1166, 24999, 25000 };

static const struct { enum plan plan; int at; const char *name; } PLANS[] = {
  { ALL_TAKEN, 0, "every write taken (TPBot Classic)" },
  { EDU_ONLY, 0, "writes to 32 taken, to 240 refused (TPBot Edu)" },
  { NONE_TAKEN, 0, "no write taken (no robot, or switched off)" },
  { ONE_REFUSED, 1, "first write refused" },
  { ONE_REFUSED, 2, "second write refused" },
  { ONE_REFUSED, 3, "third write refused" },
  { ONE_REFUSED, 4, "fourth write refused" },
  { REFUSED_FROM, 2, "writes refused from the second on" },
  { REFUSED_FROM, 3, "writes refused from the third on" },
  { REFUSED_FROM, 4, "writes refused from the fourth on" },
};

static int compare(const char *code, const char *plan, int *failures) {
  run(1, code, heard[0], sizeof heard[0]);
  run(0, code, heard[1], sizeof heard[1]);
  if (getenv("TPBOT_TEST_SHOW"))
    printf("== %s: %s\n%s\n", plan, code, heard[1]);
  if (strcmp(heard[0], heard[1]) == 0)
    return 1;
  (*failures)++;
  printf("DIFFERENT under %s: %s\n--- Lua\n%s\n--- C\n%s\n", plan, code,
         heard[0], heard[1]);
  return 0;
}

// The heap a state holds once the commands are in place, in bytes.
static int heap(int lua) {
  lua_State *L = state(lua);
  int bytes;
  lua_pushnil(L);
  lua_setglobal(L, "run_case");
  lua_gc(L, LUA_GCCOLLECT, 0);
  bytes = lua_gc(L, LUA_GCCOUNT, 0) * 1024 + lua_gc(L, LUA_GCCOUNTB, 0);
  lua_close(L);
  return bytes;
}

static char *slurp(const char *path, size_t *length) {
  FILE *f = fopen(path, "rb");
  char *text;
  if (!f) { perror(path); exit(2); }
  fseek(f, 0, SEEK_END);
  *length = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  text = malloc(*length);
  if (fread(text, 1, *length, f) != *length) { perror(path); exit(2); }
  fclose(f);
  return text;
}

int main(int argc, char **argv) {
  size_t p, e;
  int i, cases = 0, failures = 0;
  if (argc != 2) {
    fprintf(stderr, "usage: %s tests/tpbot-reference.lua\n", argv[0]);
    return 2;
  }
  reference_text = slurp(argv[1], &reference_length);
  bus.echo = -1;
  // Putting the commands in place writes nothing to the bus
  bus.log[0] = 0;
  lua_close(state(0));
  lua_close(state(1));
  if (bus.log[0]) {
    printf("the bus was written to at start:\n%s", bus.log);
    failures++;
  }
  for (p = 0; p < sizeof PLANS / sizeof PLANS[0]; p++) {
    bus.plan = PLANS[p].plan;
    bus.at = PLANS[p].at;
    for (i = 0; CALLS[i]; i++, cases++)
      compare(CALLS[i], PLANS[p].name, &failures);
  }
  bus.plan = ALL_TAKEN;
  for (e = 0; e < sizeof ECHOES / sizeof ECHOES[0]; e++) {
    char plan[32];
    bus.echo = ECHOES[e];
    snprintf(plan, sizeof plan, "echo %d us", ECHOES[e]);
    for (i = 0; SONAR[i]; i++, cases++)
      compare(SONAR[i], plan, &failures);
  }
  printf("%d cases, %d different; %d Lua messages compared without their "
         "position\n", cases, failures, positions);
  printf("heap with the commands in place: Lua %d B, C %d B "
         "(host, %d-bit pointers)\n", heap(1), heap(0),
         (int)(8 * sizeof(void *)));
  return failures != 0;
}
