// -*- mode: c; indent-tabs-mode: nil; -*-
//
// Host test for source/lua-events.c under source/lua-script.lua: boots
// the firmware's script, or a program in its place, in this firmware's Lua
// built for the host, over a stand-in board whose microbit.sleep is
// lua_events_sleep(), and runs it as main() does, between lua_call_begin()
// and lua_call_end(). Events go in through lua_event_arrived(), as the
// firmware's event fiber hands them on. The test types at the port,
// presses buttons while Lua sleeps or drives, plays the other board of a
// radio link, and checks what comes out of the port, in order.
//
// Usage: runtime-host-test source/lua-script.lua

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lua-events.h"
#include "lua-strip-debug.h"
#include "host-cstack.h"
#include "tpbot.h"

enum {
  ID_BUTTON_A = 1, ID_BUTTON_B = 2, ID_DISPLAY = 7, ID_RADIO = 9,
  ID_SERIAL = 12, HEAD_MATCH = 2, DOWN = 1, UP = 2
};

// The board: what went out of the port, in order, with marks for the
// display, the sleeps and the robot; what waits to be read from the port,
// and whether it is armed; and the clock.
static char out[65536];
static char typed[4096];
static size_t typed_at, typed_len;
static int armed;
static uint32_t clock_ms;
static lua_State *board_L;

// What arrives at the port just before it is armed, raising no event
static const char *arm_finds;

// What happens while the board pauses: one action for each pause, in turn
typedef void (*Action)(void);
static Action pauses[16];
static int pauses_planned, pauses_done;

static void note(const char *text) {
  size_t n = strlen(out);
  snprintf(out + n, sizeof out - n, "%s", text);
}

static void post(int source, int value) {
  LuaEvent e = { (uint16_t)source, (uint16_t)value, 0 };
  lua_event_arrived(e);
}

// Characters arriving at the port: the armed event fires on the first
static void type_in(const char *text) {
  size_t n = strlen(text);
  memcpy(typed + typed_len, text, n);
  typed_len += n;
  if (armed && n > 0) {
    armed = 0;
    post(ID_SERIAL, HEAD_MATCH);
  }
}

void lua_events_mistake(const char *message) {
  note("<mistake ");
  note(message);
  note(">");
}

uint32_t lua_events_now(void) {
  return clock_ms;
}

void lua_events_pause(uint32_t ms) {
  clock_ms += ms;
  if (pauses_done < pauses_planned)
    pauses[pauses_done++]();
}

static void plan(Action a) {
  pauses[pauses_planned++] = a;
}

// A robot that takes every command; robot_move's sleep is the firmware's,
// which lets other fibers run and handles no events
int tpbot_i2c_write(int address, const char *data, size_t length) {
  (void)address; (void)data; (void)length;
  return 0;
}

int tpbot_i2c_read(int address, char *data, size_t length) {
  (void)address;
  memset(data, 1, length);
  return 0;
}

void tpbot_sleep(uint32_t ms) {
  note("<move>");
  lua_events_pause(ms);
}

int tpbot_echo_us(void) { return -1; }

// The radio link's other board, which the test plays. Once listen() is
// called, a board that calls this one on its first look, types the keys
// the test gives it and hears what this one says; after a few looks the
// test presses Ctrl+C on the port, which ends listen(). Once connect() is
// called, a board that is linked: it hears what tx sends and answers with
// what the test gives it, raising the radio's event while tx waits.
static const char *caller_types;
static char caller_hears[1024];
static int listening, looks;
static char far_heard[1024];
static const char *far_says;
static int linked;

static void append(char *to, size_t size, const char *text) {
  size_t n = strlen(to);
  snprintf(to + n, size - n, "%s", text);
}

static int l_connect(lua_State *L) {
  linked = 1;
  lua_pushboolean(L, 1);
  return 1;
}

static int l_answered(lua_State *L) {
  if (!listening) {
    listening = 1;
    lua_pushliteral(L, "gigat");
    return 1;
  }
  if (++looks == 3)
    type_in("\3");
  lua_pushnil(L);
  return 1;
}

static int l_tx(lua_State *L) {
  const char *text = luaL_checkstring(L, 1);
  if (listening) {
    append(caller_hears, sizeof caller_hears, text);
  } else {
    note("<tx>");
    append(far_heard, sizeof far_heard, text);
    if (far_says)
      post(ID_RADIO, 1);
    note("</tx>");
  }
  lua_pushboolean(L, 1);
  return 1;
}

static int l_rx(lua_State *L) {
  const char *piece = listening ? caller_types : linked ? far_says : NULL;
  if (listening)
    caller_types = NULL;
  else
    far_says = NULL;
  if (piece)
    lua_pushstring(L, piece);
  else
    lua_pushnil(L);
  return 1;
}

static int l_close(lua_State *L) {
  (void)L;
  linked = 0;
  return 0;
}

static int l_send(lua_State *L) {
  note(luaL_checkstring(L, 1));
  return 0;
}

static int l_get_char(lua_State *L) {
  if (typed_at == typed_len) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, typed + typed_at++, 1);
  return 1;
}

static int l_arm(lua_State *L) {
  (void)L;
  if (arm_finds) {
    type_in(arm_finds);
    arm_finds = NULL;
  }
  armed = 1;
  return 0;
}

// A scroll takes time, letting other fibers run, and handles no events
static int l_scroll(lua_State *L) {
  note("<scroll ");
  note(luaL_checkstring(L, 1));
  note(">");
  lua_events_pause(100);
  note("</scroll>");
  return 0;
}

// microbit.sleep, as the firmware binds it
static int l_version(lua_State *L) {
  lua_pushstring(L, "hosttest");
  return 1;
}

static int l_sleep(lua_State *L) {
  uint32_t ms = (uint32_t)luaL_checkinteger(L, 1);
  note("<sleep>");
  lua_events_sleep(ms);
  note("</sleep>");
  return 0;
}

static int l_name(lua_State *L) {
  lua_pushliteral(L, "zezop");
  return 1;
}

static int l_nothing(lua_State *L) {
  (void)L;
  return 0;
}

// The modules the firmware's script requires, with the stand-ins above;
// tpbot is the firmware's own
static const luaL_Reg l_microbit[] = {
  {"sleep", l_sleep}, {"friendlyName", l_name}, {"version", l_version},
  {NULL, NULL}
};
static const struct { const char *name; int value; } constants[] = {
  {"DEVICE_ID_BUTTON_A", ID_BUTTON_A}, {"DEVICE_ID_BUTTON_B", ID_BUTTON_B},
  {"MICROBIT_ID_LOGO", 121}, {"DEVICE_ID_DISPLAY", ID_DISPLAY},
  {"DEVICE_ID_GESTURE", 27}, {"DEVICE_ID_RADIO", ID_RADIO},
  {"DEVICE_ID_SERIAL", ID_SERIAL}, {"CODAL_SERIAL_EVT_HEAD_MATCH", HEAD_MATCH},
  {"DEVICE_BUTTON_EVT_DOWN", DOWN}, {"DEVICE_BUTTON_EVT_UP", UP},
  {"DISPLAY_EVT_ANIMATION_COMPLETE", 1}, {"ACCELEROMETER_EVT_TILT_UP", 1},
  {"ACCELEROMETER_EVT_TILT_DOWN", 2}, {"ACCELEROMETER_EVT_TILT_LEFT", 3},
  {"ACCELEROMETER_EVT_TILT_RIGHT", 4}, {"ACCELEROMETER_EVT_FACE_UP", 5},
  {"ACCELEROMETER_EVT_SHAKE", 11}, {NULL, 0}
};
static const luaL_Reg l_display[] = {
  {"animate", l_nothing}, {"scrollAsync", l_nothing}, {"show", l_nothing},
  {"scroll", l_scroll}, {NULL, NULL}
};
static const luaL_Reg l_serial[] = {
  {"send", l_send}, {"getCharAsync", l_get_char},
  {"eventAfterAsync", l_arm}, {NULL, NULL}
};
static const luaL_Reg l_radio[] = {
  {"enable", l_nothing}, {"beacon", l_nothing}, {"connect", l_connect},
  {"answered", l_answered}, {"tx", l_tx}, {"rx", l_rx}, {"close", l_close},
  {NULL, NULL}
};
static const luaL_Reg l_audio[] = { {"express", l_nothing}, {NULL, NULL} };
static const luaL_Reg l_accelerometer[] = {
  {"getSample", l_nothing}, {NULL, NULL}
};
static const luaL_Reg l_none[] = { {NULL, NULL} };
#define X(name, function) {#name, function},
static const luaL_Reg l_tpbot[] = { TPBOT_FUNCTIONS {NULL, NULL} };
#undef X

// require(name): a module made at boot, below, or tpbot, made when first
// required, as the firmware's require makes it
static int l_require(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
  lua_getfield(L, -1, name);
  if (lua_isnil(L, -1) && strcmp(name, "tpbot") == 0) {
    luaL_register(L, "tpbot", l_tpbot);
    tpbot_register_globals(L);
  }
  if (lua_isnil(L, -1))
    return luaL_error(L, "module '%s' not found", name);
  return 1;
}

static void modules(lua_State *L) {
  int i;
  luaL_register(L, "microbit", l_microbit);
  for (i = 0; constants[i].name; i++) {
    lua_pushinteger(L, constants[i].value);
    lua_setfield(L, -2, constants[i].name);
  }
  luaL_register(L, "microbit.display", l_display);
  luaL_register(L, "microbit.serial", l_serial);
  luaL_register(L, "microbit.radio", l_radio);
  luaL_register(L, "microbit.audio", l_audio);
  luaL_register(L, "microbit.accelerometer", l_accelerometer);
  luaL_register(L, "microbit.io", l_none);
  lua_settop(L, 0);
  lua_register(L, "require", l_require);
}

static void fresh(void) {
  out[0] = 0;
  typed_at = typed_len = 0;
  armed = 0;
  clock_ms = 0;
  arm_finds = NULL;
  pauses_planned = pauses_done = 0;
  caller_types = NULL;
  caller_hears[0] = far_heard[0] = 0;
  listening = looks = linked = 0;
  far_says = NULL;
}

// Boot the board with text as its script, loaded and stripped as main()
// does
static void boot_script(const char *text, size_t length) {
  lua_State *L;
  if (board_L)
    lua_close(board_L);
  board_L = L = luaL_newstate();
  luaopen_base(L);
  luaopen_table(L);
  luaopen_string(L);
  luaopen_math(L);
  lua_settop(L, 0);
  modules(L);
  lua_events_open(L, ID_SERIAL, HEAD_MATCH);
  if (luaL_loadbuffer(L, text, length, "embedded") != 0) {
    fprintf(stderr, "the script did not load: %s\n", lua_tostring(L, -1));
    exit(2);
  }
  lua_strip_debug(L);
  lua_call_begin();
  if (lua_pcall(L, 0, 0, 0) != 0) {
    fprintf(stderr, "the script stopped: %s\n", lua_tostring(L, -1));
    exit(2);
  }
  lua_call_end();
  note("<booted>");
}

static char *script;
static size_t script_len;

static void boot(void) {
  boot_script(script, script_len);
}

// A program that is the board's whole script
static void boot_alone(const char *program) {
  boot_script(program, strlen(program));
}

static int failures, checks;

static void expect(int ok, const char *what) {
  checks++;
  if (!ok) {
    failures++;
    printf("FAIL %s\n  port: %s\n", what, out);
  } else {
    printf("ok   %s\n", what);
  }
}

static int count(const char *text, const char *part) {
  int n = 0;
  const char *p = text;
  while ((p = strstr(p, part)) != NULL) { n++; p++; }
  return n;
}

// Whether the parts come in text in this order
static int in_order(const char *text, const char *const *parts) {
  for (; *parts; parts++) {
    text = strstr(text, *parts);
    if (!text) return 0;
    text += strlen(*parts);
  }
  return 1;
}

// Type a line at the prompt; what came out since
static const char *line(const char *text) {
  static char said[65536];
  size_t before = strlen(out);
  type_in(text);
  type_in("\r");
  snprintf(said, sizeof said, "%s", out + before);
  return said;
}

static void press_a(void) { post(ID_BUTTON_A, DOWN); }
static void press_b(void) { post(ID_BUTTON_B, DOWN); }
static void press_a_then_b(void) { press_a(); press_b(); }
static void type_a_line(void) { type_in("print(2)\r"); }

static void press_twenty(void) {
  int i;
  for (i = 0; i < 20; i++)
    press_a();
}

// Handlers for A and B, set at the prompt, that say when they run
static void say_the_buttons(void) {
  line("microbit.handler[microbit.DEVICE_ID_BUTTON_A] = "
       "function() print('A') end");
  line("microbit.handler[microbit.DEVICE_ID_BUTTON_B] = "
       "function() print('B') end");
}

static void the_repl(void) {
  const char *said;
  fresh();
  boot();
  expect(strstr(out, "micro:bit\r\nLua 5.1 REPL\r\nfirmware hosttest\r\n> ") == out,
         "the greeting, then the prompt");
  expect(armed, "the port is armed after boot");
  lua_getfield(board_L, LUA_REGISTRYINDEX, "_LOADED");
  lua_getfield(board_L, -1, "tpbot");
  expect(lua_isnil(board_L, -1), "booting makes no tpbot: it comes with "
         "require(\"tpbot\")");
  lua_settop(board_L, 0);
  said = line("6*7");
  expect(strcmp(said, "6*7\r\n=> 42\r\n> ") == 0, "a line is answered");
  said = line("error('boom')");
  expect(strstr(said, "Runtime error: [string \"REPL\"]:1: boom\r\n> ")
         != NULL, "a mistake at the prompt is said, and the prompt is back");

  // What was typed while the script ran is read once it has
  fresh();
  type_in("1+1\r");
  boot();
  expect(strstr(out, "> 1+1\r\n=> 2\r\n> ") != NULL && armed,
         "a line typed while the script at boot ran is answered after it");

  // A line that reaches the port just before it is armed raises no event
  fresh();
  boot();
  arm_finds = "2+2\r";
  type_in("1\r");
  expect(strstr(out, "=> 1\r\n> 2+2\r\n=> 4\r\n> ") != NULL && armed,
         "a line that came before the port was armed is read all the same");
}

static void one_call_at_a_time(void) {
  const char *said;
  {
    static const char *const order[] = {
      "<sleep>", "A\r\n", "</sleep>", "> ", NULL };
    fresh();
    boot();
    say_the_buttons();
    plan(press_a);
    said = line("microbit.sleep(50)");
    expect(in_order(said, order) && count(said, "A\r\n") == 1,
           "a press while a line sleeps is handled in its sleep, once");
  }
  {
    static const char *const order[] = { "A\r\n", "B\r\n", "</sleep>", NULL };
    fresh();
    boot();
    say_the_buttons();
    plan(press_a_then_b);
    said = line("microbit.sleep(50)");
    expect(in_order(said, order), "presses are handled in the order they came");
  }
  {
    // A's handler sleeps; B, pressed meanwhile, waits for it to end
    static const char *const order[] = {
      "A in", "<sleep>", "</sleep>", "A out", "B\r\n", NULL };
    fresh();
    boot();
    say_the_buttons();
    line("microbit.handler[microbit.DEVICE_ID_BUTTON_A] = function() "
         "print('A in') microbit.sleep(20) print('A out') end");
    plan(press_a);
    plan(press_b);
    said = line("microbit.sleep(50)");
    expect(in_order(said, order),
           "a handler's own sleep handles nothing: the next one waits");
  }
  {
    static const char *const order[] = { "<move>", "> ", "A\r\n", NULL };
    fresh();
    boot();
    say_the_buttons();
    line("require('tpbot')");
    plan(press_a);
    said = line("robot_move(40, 40, 1)");
    expect(in_order(said, order),
           "a press while the robot drives is handled once the line is over");
  }
  fresh();
  boot();
  say_the_buttons();
  line("require('tpbot')");
  plan(press_twenty);
  said = line("robot_move(40, 40, 1)");
  expect(count(said, "A\r\n") == 16,
         "of twenty presses while Lua is busy, sixteen wait and are handled");
  {
    static const char *const order[] = {
      "<sleep>", "</sleep>", "print(2)\r\n2\r\n> ", NULL };
    fresh();
    boot();
    plan(type_a_line);
    line("microbit.sleep(50)");
    expect(in_order(out, order),
           "a line typed while a line sleeps runs after it, not inside it");
  }
  {
    // A's handler scrolls, a wait; a line typed at the prompt meanwhile
    static const char *const order[] = {
      "<scroll hi>", "</scroll>", "print(2)\r\n2\r\n> ", NULL };
    fresh();
    boot();
    line("microbit.handler[microbit.DEVICE_ID_BUTTON_A] = function() "
         "microbit.display.scroll('hi') end");
    plan(type_a_line);
    press_a();
    expect(in_order(out, order),
           "a line typed while a handler scrolls runs once the handler ends");
  }
}

static void mistakes_in_handlers(void) {
  const char *said;
  {
    fresh();
    boot();
    line("microbit.handler[microbit.DEVICE_ID_BUTTON_A] = "
         "function() error('oops') end");
    press_a();
    expect(strstr(out, "<mistake [string \"REPL\"]:1: oops>") != NULL,
           "a mistake in a handler is said");
    said = line("6*7");
    expect(strstr(said, "=> 42\r\n> ") != NULL, "... and the REPL answers");
  }
  {
    static const char *const order[] = {
      "<sleep>", "<mistake", "</sleep>", "after\r\n> ", NULL };
    fresh();
    boot();
    line("microbit.handler[microbit.DEVICE_ID_BUTTON_A] = "
         "function() error('oops') end");
    plan(press_a);
    said = line("microbit.sleep(50) print('after')");
    expect(in_order(said, order),
           "a mistake in a handler run in a line's sleep: the line goes on");
  }
}

// A program's own on_event, set at the prompt, that passes what it does
// not handle on to the REPL's
static void a_program_s_on_event(void) {
  static const char FORWARD[] =
    "local original = on_event "
    "_G.on_event = function(s, v, t) "
    "if s == microbit.DEVICE_ID_BUTTON_A then print('mine') "
    "else original(s, v, t) end end";
  static const char *const order[] = {
    "<sleep>", "mine\r\n", "</sleep>", "> ", NULL };
  const char *said;
  fresh();
  boot();
  line(FORWARD);
  press_a();
  said = line("6*7");
  expect(strstr(out, "mine\r\n") != NULL && strstr(said, "=> 42\r\n> ") != NULL,
         "a program's on_event takes a press, and the REPL still answers");
  plan(press_a);
  said = line("microbit.sleep(50)");
  expect(in_order(said, order),
         "... and a line typed through it handles a press in its sleep");
}

// A program that is the board's whole script: it sets on_event, then
// loops with microbit.sleep
static void a_program_alone(void) {
  static const char *const order[] = {
    "<sleep>", "got 1", "</sleep>", "<sleep>", "got 2", "</sleep>",
    "<booted>", NULL };
  fresh();
  plan(press_a);
  plan(press_b);
  boot_alone("require('microbit')\n"
             "local send = require('microbit.serial').send\n"
             "function on_event(s, v) send('got ' .. s) end\n"
             "for i = 1, 2 do microbit.sleep(10) end\n");
  expect(in_order(out, order),
         "a program at boot gets each press in the sleep it came in");
}

// A mistake in a handler of the script at boot, stripped of its line info
// as main() strips it, names the chunk and no line: neither a line 0, nor
// nothing at all
static void mistakes_named(void) {
  fresh();
  boot_alone("require('microbit')\n"
             "function on_event(s) if s == 1 then go_forward() "
             "else error('boom') end end\n");
  press_a();
  press_b();
  expect(strstr(out, "<mistake [string \"embedded\"]: attempt to call "
                "global 'go_forward' (a nil value)>") != NULL,
         "a mistake Lua finds in a stripped handler names its chunk, no "
         "line 0");
  expect(strstr(out, "<mistake [string \"embedded\"]: boom>") != NULL,
         "error() in a stripped handler names its chunk too");
}

// What the radio link needs of the engine: the keys go over, and what the
// other board says, whose event comes while tx waits, reaches the port
// once the line's call is over
static void the_radio_link(void) {
  static const char *const order[] = { "</tx>", "=> 2\r\n> ", NULL };
  const char *said;
  fresh();
  boot();
  caller_types = "6*7\r";
  said = line("listen('gigat')");
  expect(strcmp(caller_hears, "> 6*7\r\n=> 42\r\n> ") == 0,
         "listen() serves a board that calls: a prompt, its keys echoed, "
         "its line answered");
  expect(armed && strlen(said) > 2
         && strcmp(said + strlen(said) - 2, "> ") == 0,
         "... and Ctrl+C on the port ends it: the prompt is back, the port "
         "armed");

  // A mistyped name is said, and the next listen() serves
  fresh();
  boot();
  line("listen('gigt')");
  caller_types = "6*7\r";
  line("listen('gigat')");
  expect(strstr(out, "Runtime error: a board name is five letters") != NULL
         && strcmp(caller_hears, "> 6*7\r\n=> 42\r\n> ") == 0,
         "a mistyped name in listen() is said, and the next listen() serves");

  fresh();
  boot();
  said = line("connect('gigat')");
  expect(linked && strstr(said, "gigat connected.\r\n") != NULL,
         "connect() reaches the other board");
  far_says = "1+1\r\n=> 2\r\n> ";
  said = line("1+1");
  expect(strcmp(far_heard, "1+1\r") == 0 && in_order(said, order),
         "what is typed goes over the link, and the answer, which came while "
         "tx waited, reaches the port once the line's call is over");
  said = line("\4");
  expect(!linked && strstr(said, "gigat disconnected.\r\n> ") != NULL,
         "Ctrl+D hands the port back to the console");
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL, "... which answers again");
}

static char *slurp(const char *path, size_t *length) {
  FILE *f = fopen(path, "rb");
  char *text;
  if (!f) { perror(path); exit(2); }
  fseek(f, 0, SEEK_END);
  *length = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  text = malloc(*length + 1);
  if (fread(text, 1, *length, f) != *length) { perror(path); exit(2); }
  text[*length] = 0;
  fclose(f);
  return text;
}

int main(int argc, char **argv) {
  host_cstack_start();
  if (argc != 2) {
    fprintf(stderr, "usage: %s source/lua-script.lua\n", argv[0]);
    return 2;
  }
  script = slurp(argv[1], &script_len);
  the_repl();
  one_call_at_a_time();
  mistakes_in_handlers();
  a_program_s_on_event();
  a_program_alone();
  mistakes_named();
  the_radio_link();
  printf("%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
