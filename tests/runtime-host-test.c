// -*- mode: c; indent-tabs-mode: nil; -*-
//
// Host test for source/lua-events.c and source/lua-script.lua: boots a
// script, loaded and stripped as the firmware loads it and run through
// lua_events_boot(), in this firmware's Lua built for the host, over a
// stand-in board whose microbit.sleep is lua_events_sleep(). Events are
// handed in through the firmware's own dispatcher. It boots the firmware's
// own script, and programs that stand alone in its place, as upload() puts
// them; presses buttons and types while Lua sleeps; takes away the
// globals the REPL uses; and checks what comes out of the port and onto
// the display, in order.
//
// Usage: runtime-host-test source/lua-script.lua

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lua-events.h"
#include "lua-modules.h"
#include "board-alloc.h"
#include "radio-inbox.h"
#include "host-cstack.h"
#include "tpbot.h"

void lua_strip_debug(lua_State *L);

enum {
  ID_BUTTON_A = 1, ID_BUTTON_B = 2, ID_BUTTON_AB = 3, ID_DISPLAY = 7,
  ID_RADIO = 9, ID_SERIAL = 12, HEAD_MATCH = 2, CLICK = 3, LONG_CLICK = 4,
  RX_FULL = 3, DATA_RECEIVED = 4, ANIMATION_COMPLETE = 1
};

// The events the firmware lists as noise, and its port's event
static const LuaEventId noise[] = {
  { ID_SERIAL, DATA_RECEIVED }, { ID_SERIAL, RX_FULL },
  { ID_DISPLAY, ANIMATION_COMPLETE },
};
static const LuaEventsConfig events_config = {
  { ID_SERIAL, HEAD_MATCH }, noise, sizeof noise / sizeof noise[0]
};

// The board's heap outside Lua's: what was made from it, and how much
static int board_allocs;
static size_t board_bytes;

void *board_alloc(size_t size) {
  board_allocs++;
  board_bytes += size;
  return malloc(size);
}

void board_free(void *p) {
  free(p);
}

// The board: what went out of the port and onto the display, in order,
// what waits to be read from the port, whether the port is armed, and the
// clock.
static char out[65536];
static char typed[4096];
static size_t typed_at, typed_len;
static int armed;
static uint32_t clock_ms;
static lua_State *board_L;

// What happens while the board pauses: one action for each pause, in
// order; and one while it shows a mistake.
typedef void (*Action)(void);
static Action pauses[16];
static int pauses_planned, pauses_done;
static Action on_error;

static void note(const char *text) {
  size_t n = strlen(out);
  snprintf(out + n, sizeof out - n, "%s", text);
}

static void post(int source, int value) {
  LuaEvent e = { (uint16_t)source, (uint16_t)value, 0 };
  lua_event_arrived(e);
}

// The port holds 254 characters, as the firmware sets it; what comes when
// it is full is lost, as CODAL loses it.
#define PORT_HOLDS 254
static size_t lost;

// Characters arriving at the port: the armed event fires on the first.
static void type_in(const char *text) {
  size_t n = strlen(text), i;
  for (i = 0; i < n; i++) {
    if (typed_len - typed_at == PORT_HOLDS) {
      lost++;
      continue;
    }
    typed[typed_len++] = text[i];
  }
  if (armed && n > 0) {
    armed = 0;
    post(ID_SERIAL, HEAD_MATCH);
  }
}

// Text sent over the cable as it goes, a few characters at a time
static void send_paced(const char *text, size_t at_a_time) {
  char piece[64];
  size_t n = strlen(text), i;
  for (i = 0; i < n; i += at_a_time) {
    size_t k = n - i < at_a_time ? n - i : at_a_time;
    memcpy(piece, text + i, k);
    piece[k] = 0;
    type_in(piece);
  }
}

void lua_events_show_error(const char *message, bool wait) {
  Action then = on_error;
  on_error = NULL;
  note(wait ? "<error " : "<error, going on ");
  note(message);
  note(">");
  if (then)
    then();
}

// The stack in use a safe point sees: 0, or what a test says
static uint32_t stack_used_now;

uint32_t lua_events_stack_used(void) {
  return stack_used_now;
}

uint32_t lua_events_now(void) {
  return clock_ms;
}

static int pauses_all;

void lua_events_pause(uint32_t ms) {
  pauses_all++;
  clock_ms += ms;
  if (pauses_done < pauses_planned)
    pauses[pauses_done++]();
}

// A robot that takes every command; robot_move's sleep is the firmware's,
// which lets other fibers run and handles no events.
static int bus_writes;

int tpbot_i2c_write(int address, const char *data, size_t length) {
  (void)address; (void)data; (void)length;
  bus_writes++;
  return 0;
}
// robot_move's sleep, as the firmware binds it
void tpbot_sleep(uint32_t ms) {
  char text[32];
  lua_events_before_wait();
  snprintf(text, sizeof text, "<move %lu>", (unsigned long)ms);
  note(text);
  lua_events_pause(ms);
}
int tpbot_echo_us(void) { return -1; }

// A radio link whose far end is a board that answers each line with
// "=> " and the line: what tx sends comes back through rx, with the
// radio's event.
static char far_end[1024];
static int linked;

static int l_connect(lua_State *L) {
  linked = 1;
  lua_pushboolean(L, 1);
  return 1;
}

static int l_tx(lua_State *L) {
  const char *text = luaL_checkstring(L, 1);
  size_t n = strlen(far_end);
  snprintf(far_end + n, sizeof far_end - n, "=> %s\n", text);
  lua_pushboolean(L, 1);
  post(ID_RADIO, 1);
  return 1;
}

static int l_rx(lua_State *L) {
  if (!linked || !far_end[0]) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, far_end);
  far_end[0] = 0;
  return 1;
}

// What the port does on the next send of a given text: take in more
// characters, unarmed, as if they came while it sent; or fail once.
static const char *send_trigger, *send_brings;
static int send_fails;

static int l_send(lua_State *L) {
  const char *text = luaL_checkstring(L, 1);
  if (send_fails) {
    send_fails = 0;
    return luaL_error(L, "the port failed");
  }
  note(text);
  if (send_trigger && strcmp(text, send_trigger) == 0) {
    size_t n = strlen(send_brings);
    memcpy(typed + typed_len, send_brings, n);
    typed_len += n;
    send_trigger = NULL;
  }
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
  armed = 1;
  return 0;
}

// A scroll takes time, letting other fibers run, and says when it is over
static int l_scroll(lua_State *L) {
  note("<scroll ");
  note(luaL_checkstring(L, 1));
  note(">");
  lua_events_pause(10);
  post(ID_DISPLAY, ANIMATION_COMPLETE);
  return 0;
}

// Not the firmware's: lets a program post an event, as a sensor would
static int posts_left;

static int l_post(lua_State *L) {
  if (posts_left > 0) {
    posts_left--;
    post(luaL_checkint(L, 1), luaL_checkint(L, 2));
  }
  return 0;
}

// microbit.sleep, as the firmware binds it
static int l_sleep(lua_State *L) {
  uint32_t ms = (uint32_t)luaL_checkinteger(L, 1);
  note("<sleep>");
  lua_events_sleep(ms);
  note("</sleep>");
  return 0;
}

static int l_version(lua_State *L) {
  lua_pushliteral(L, "hosttest");
  return 1;
}

static int l_nothing(lua_State *L) {
  (void)L;
  return 0;
}

static int l_name(lua_State *L) {
  lua_pushliteral(L, "zezop");
  return 1;
}

// The board's modules, as the firmware lists them for require(), with the
// stand-ins above; tpbot is the firmware's own.
static const LuaApi l_microbit[] = {
  {"version", l_version, 0}, {"friendlyName", l_name, 0},
  {"sleep", l_sleep, 0}, {"eventsDropped", lua_events_dropped, 0},
  {"eventFallback", lua_events_fallback, 0}, {"post", l_post, 0},
  {"eventLine", lua_events_line, 0}, {"eventRepl", lua_events_repl, 0},
  {"DEVICE_ID_BUTTON_A", NULL, ID_BUTTON_A},
  {"DEVICE_ID_BUTTON_B", NULL, ID_BUTTON_B},
  {"DEVICE_ID_BUTTON_AB", NULL, ID_BUTTON_AB},
  {"DEVICE_ID_RADIO", NULL, ID_RADIO},
  {"DEVICE_ID_SERIAL", NULL, ID_SERIAL},
  {"CODAL_SERIAL_EVT_HEAD_MATCH", NULL, HEAD_MATCH},
  {"DEVICE_BUTTON_EVT_CLICK", NULL, CLICK},
  {"DEVICE_BUTTON_EVT_LONG_CLICK", NULL, LONG_CLICK},
  {NULL, NULL, 0}
};
static const LuaApi l_audio[] = {
  {"setVolume", l_nothing, 0}, {"express", l_nothing, 0}, {NULL, NULL, 0}
};
static const LuaApi l_display[] = {
  {"animate", l_nothing, 0}, {"scrollAsync", l_nothing, 0},
  {"scroll", l_scroll, 0}, {NULL, NULL, 0}
};
static const LuaApi l_serial[] = {
  {"send", l_send, 0}, {"getCharAsync", l_get_char, 0},
  {"eventAfterAsync", l_arm, 0}, {NULL, NULL, 0}
};
static const LuaApi l_radio[] = {
  {"enable", l_nothing, 0}, {"listen", l_nothing, 0},
  {"connect", l_connect, 0}, {"answered", l_nothing, 0},
  {"tx", l_tx, 0}, {"rx", l_rx, 0}, {NULL, NULL, 0}
};
#define X(name, function) {#name, function, 0},
static const LuaApi l_tpbot[] = { TPBOT_FUNCTIONS {NULL, NULL, 0} };
#undef X
static const LuaApi l_none[] = { {NULL, NULL, 0} };
static const LuaModule modules[] = {
  {"microbit", l_microbit, NULL}, {"microbit.audio", l_audio, NULL},
  {"microbit.accelerometer", l_none, NULL},
  {"microbit.compass", l_none, NULL}, {"microbit.io", l_none, NULL},
  {"microbit.i2c", l_none, NULL},
  {"microbit.display", l_display, NULL}, {"microbit.serial", l_serial, NULL},
  {"microbit.radio", l_radio, NULL},
  {"tpbot", l_tpbot, tpbot_register_globals},
  {NULL, NULL, NULL}
};

// What the next pauses do, from the next one on
static void plan(Action a) {
  pauses[pauses_planned++] = a;
}

static void fresh(void) {
  stack_used_now = 0;
  board_allocs = 0;
  board_bytes = 0;
  pauses_all = 0;
  send_trigger = NULL;
  send_fails = 0;
  posts_left = 1000;
  bus_writes = 0;
  lost = 0;
  linked = 0;
  far_end[0] = 0;
  out[0] = 0;
  typed_at = typed_len = 0;
  armed = 0;
  clock_ms = 0;
  pauses_planned = pauses_done = 0;
  on_error = NULL;
}

// The heap: how much Lua holds, and how much it may, for the tests that
// run it out
static size_t heap_used, heap_limit = (size_t)-1;

static void *alloc(void *ud, void *ptr, size_t old, size_t size) {
  (void)ud;
  if (size == 0) {
    free(ptr);
    heap_used -= old;
    return NULL;
  }
  if (size > old && heap_used + (size - old) > heap_limit)
    return NULL;
  ptr = realloc(ptr, size);
  if (ptr) heap_used += size - old;
  return ptr;
}

static int panicked;

static int panic(lua_State *L) {
  printf("PANIC %s\n", lua_tostring(L, -1));
  panicked++;
  exit(3);
}

// Boot the board with text as its script, as the firmware does
static void boot_script(const char *text, size_t length) {
  lua_State *L;
  if (board_L) lua_close(board_L);
  heap_used = 0;
  heap_limit = (size_t)-1;
  board_L = L = lua_newstate(alloc, NULL);
  lua_atpanic(L, panic);
  luaopen_base(L);
  luaopen_table(L);
  luaopen_string(L);
  luaopen_math(L);
  lua_settop(L, 0);
  lua_modules_open(L, modules);
  lua_events_open(L, &events_config);
  if (luaL_loadbuffer(L, text, length, "embedded")) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    exit(2);
  }
  lua_strip_debug(L);
  lua_events_boot(L);
  note("<booted>");
}

static char *script;
static size_t script_len;

// The firmware's own script, with program in it where a file used to go:
// before its last serial_session.prompt()
static void boot(const char *program) {
  static const char PROMPT[] = "\nserial_session.prompt()";
  char *text, *at = NULL, *p = script;
  size_t head, n = strlen(program);
  while ((p = strstr(p, PROMPT)) != NULL) at = p++;
  if (!at) { fprintf(stderr, "no prompt line\n"); exit(2); }
  head = (size_t)(at - script) + 1;
  text = malloc(script_len + n + 2);
  memcpy(text, script, head);
  memcpy(text + head, program, n);
  text[head + n] = '\n';
  memcpy(text + head + n + 1, script + head, script_len - head);
  boot_script(text, script_len + n + 1);
  free(text);
}

// A program that is the board's whole script, as upload() puts one
static void boot_alone(const char *program) {
  boot_script(program, strlen(program));
}

// Such a program that starts by requiring microbit and its display
static void boot_program(const char *program) {
  static const char START[] =
    "require('microbit') require('microbit.display')\n";
  char text[4096];
  snprintf(text, sizeof text, "%s%s", START, program);
  boot_alone(text);
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

static void press_a(void) { post(ID_BUTTON_A, CLICK); }
static void press_b(void) { post(ID_BUTTON_B, CLICK); }
static void press_b_then_a(void) { press_b(); press_a(); }

static void press_three(void) {
  post(ID_BUTTON_A, CLICK);
  post(ID_BUTTON_B, CLICK);
  post(ID_BUTTON_AB, CLICK);
}

// The port's event, with a line, while a command sleeps
static void type_and_post_the_port(void) {
  memcpy(typed + typed_len, "print(3)\r", 9);
  typed_len += 9;
  post(ID_SERIAL, HEAD_MATCH);
}

static void type_a_sleeping_line(void) {
  type_in("microbit.sleep(50)\r");
}

// A line typed at the armed prompt, whose port event is lost on its way
static void type_and_miss_the_port(void) {
  memcpy(typed + typed_len, "6*7\r", 4);
  typed_len += 4;
  armed = 0;
  lua_events_port_missed();
}

static void press_a_and_type(void) {
  press_a();
  type_in("print(6*7)\r");
}

// Four presses of A, then sixteen of B: one more than the line holds
static void flood(void) {
  int i;
  for (i = 0; i < 4; i++)
    post(ID_BUTTON_A, CLICK);
  for (i = 0; i < 16; i++)
    post(ID_BUTTON_B, CLICK);
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

static const char *GLOBALS[] = {
  "pcall", "print", "tostring", "type", "select", "pairs", "ipairs",
  "next", "setmetatable", "getmetatable", "loadstring", "setfenv",
  "collectgarbage", "string", "table", "unpack", "error", "rawget",
  "rawset", "tonumber", "assert", "io", "microbit", "active_session",
  "require", NULL
};

static const char *LIBRARY_FIELDS[] = {
  "string.format", "string.find", "string.gmatch", "string.match",
  "string.sub", "table.insert", "table.concat", "microbit.display.scroll",
  "microbit.serial.send", "microbit.serial.getCharAsync",
  "microbit.serial.eventAfterAsync", "microbit.handler",
  "microbit.radio.enable", "microbit.radio.connect", "microbit.radio.tx",
  "microbit.radio.rx", "microbit.sleep", NULL
};

// Every way into the runtime's handlers still works: a line, a line with
// a backspace, a button, and a line over a radio link
static void still_answers(const char *after) {
  char what[256];
  const char *said = line("'hi', {a = 1}");
  snprintf(what, sizeof what, "the REPL answers after %s", after);
  expect(strstr(said, "=> \"hi\"\t{a = 1}\r\n> ") != NULL, what);
  said = line("6*8\b7");
  snprintf(what, sizeof what, "... a backspace, after %s", after);
  expect(strstr(said, "=> 42\r\n> ") != NULL, what);
  {
    size_t before = strlen(out);
    press_a();
    snprintf(what, sizeof what, "... a button, after %s", after);
    expect(strstr(out + before, "<scroll A>") != NULL, what);
  }
  line("connect('zezop', 10)");
  said = line("1+1");
  snprintf(what, sizeof what, "... a line over the radio, after %s", after);
  expect(strstr(said, "=> 1+1") != NULL, what);
}

static void the_repl(void) {
  const char *said;
  fresh();
  boot("");
  expect(strstr(out, "micro:bit\r\nLua 5.1 REPL\r\nfirmware hosttest\r\n> ")
         == out, "the greeting, then the prompt");
  expect(armed, "the port is armed after boot");
  expect(bus_writes == 0, "booting writes nothing to the bus");
  said = line("print(6*7)");
  expect(strcmp(said, "print(6*7)\r\r\n42\r\n> ") == 0, "print(6*7)");
  said = line("6*7");
  expect(strcmp(said, "6*7\r\r\n=> 42\r\n> ") == 0, "6*7");
  said = line("error('boom')");
  expect(strstr(said, "Runtime error: [string \"REPL\"]:1: boom\r\n> ") != NULL,
         "a mistake at the prompt");
}

static void events_and_the_repl(void) {
  const char *said;
  {
    // A program that sleeps at boot: a press meanwhile is handled in the
    // sleep, once; a line typed meanwhile runs at the prompt
    static const char *const order[] = {
      "<sleep>", "<scroll A>", "</sleep>", "> print(6*7)\r\r\n42\r\n> ",
      "<booted>", NULL };
    fresh();
    plan(press_a_and_type);
    boot("microbit.sleep(100)");
    expect(count(out, "<scroll A>") == 1 && in_order(out, order),
           "a press during a program's sleep at boot is handled in it, "
           "once; a line typed then runs at the prompt");
  }
  {
    // A command that sleeps handles presses in its sleep, in order
    static const char *const order[] = {
      "<sleep>", "<scroll A>", "<scroll B>", "<scroll AB>", "</sleep>",
      "> ", NULL };
    fresh();
    boot("");
    plan(press_three);
    said = line("microbit.sleep(50)");
    expect(count(said, "<scroll") == 3 && in_order(said, order),
           "presses during a command's sleep, handled in it, in order");
  }
  // More presses than the line holds: the oldest are dropped, and counted
  fresh();
  boot("");
  plan(flood);
  said = line("microbit.sleep(50)");
  expect(count(said, "<scroll B>") == 16 && count(said, "<scroll A>") == 0,
         "of twenty presses at once, the newest sixteen are kept");
  said = line("microbit.eventsDropped()");
  expect(strstr(said, "=> 4\r\n") != NULL, "four counted as dropped");
}

static void on_event_and_print(void) {
  const char *said;
  fresh();
  boot("");
  line("_G.on_event = 1");
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL, "on_event = 1 leaves the REPL");
  fresh();
  boot("");
  line("_G.on_event = nil");
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL, "on_event = nil leaves the REPL");
  fresh();
  boot("on_event = 'x'");
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL,
         "on_event = 'x' from a program at boot leaves the REPL");
  fresh();
  boot("function on_event(s, v) microbit.display.scroll('mine') end");
  line("6*7");
  expect(count(out, "<scroll mine>") == 1 && strstr(out, "=> 42") == NULL,
         "a program's own on_event takes every event");

  fresh();
  boot("");
  line("function print() io.write('mine\\n') end");
  said = line("print(1)");
  expect(strstr(said, "mine\r\n> ") != NULL, "a print of one's own is used");
  said = line("1+1");
  expect(strstr(said, "=> 2\r\n> ") != NULL, "... and the REPL still answers");
}

// The REPL keeps only pcall of its own: a global it uses, taken away,
// can stop what a line shows, but the prompt comes back and the port is
// armed, so the global can be put back from the prompt. print is the
// REPL's own too.
static void globals_taken_away(void) {
  const char *said;
  char what[256], take[128];
  int i;
  fresh();
  boot("");
  still_answers("nothing");
  for (i = 0; GLOBALS[i]; i++) {
    fresh();
    boot("");
    snprintf(take, sizeof take, "_G.%s = nil", GLOBALS[i]);
    line(take);
    said = line("6*7");
    snprintf(what, sizeof what, "the prompt comes back after %s", take);
    expect(armed && strstr(said, "> ") != NULL, what);
  }
  for (i = 0; LIBRARY_FIELDS[i]; i++) {
    fresh();
    boot("");
    snprintf(take, sizeof take, "%s = nil", LIBRARY_FIELDS[i]);
    line(take);
    said = line("6*7");
    snprintf(what, sizeof what, "the prompt comes back after %s", take);
    expect(armed && strstr(said, "> ") != NULL, what);
  }
  for (i = 0; GLOBALS[i]; i++) {
    fresh();
    snprintf(take, sizeof take, "_G.%s = nil", GLOBALS[i]);
    boot(take);
    said = line("6*7");
    snprintf(what, sizeof what, "the prompt comes back after a program's %s",
             take);
    expect(armed && strstr(said, "> ") != NULL, what);
  }
  fresh();
  boot("");
  line("_G.pcall = nil");
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL, "pcall = nil leaves the REPL whole");
  line("_G.print = nil");
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL, "print = nil leaves the REPL whole");
  line("_G.microbit.display.scroll = nil");
  {
    size_t before = strlen(out);
    press_a();
    expect(strstr(out + before, "<scroll A>") != NULL,
           "a module's function set to nil comes back from the firmware");
  }
}

// Heap left for a line whose result, shown, needs more: enough to compile
// the line and run it, too little for the text of 2000 numbers
#ifndef TOO_BIG_MARGIN
#define TOO_BIG_MARGIN 8000
#endif

// Nothing on the way from a line to the prompt leaves the port unarmed
static void the_repl_always_comes_back(void) {
  static const char *const BAD[] = {
    "error(setmetatable({}, { __tostring = function() error('x') end }))",
    "error(setmetatable({}, { __tostring = function() return {} end }))",
    "local p = newproxy(true) "
    "getmetatable(p).__tostring = function() error('x') end return p",
    NULL };
  const char *said;
  char what[256];
  int i;
  for (i = 0; BAD[i]; i++) {
    fresh();
    boot("");
    line(BAD[i]);
    said = line("print(6*7)");
    snprintf(what, sizeof what, "the prompt comes back after %s", BAD[i]);
    expect(armed && strstr(said, "42\r\n> ") != NULL, what);
  }
  // A result too big to show: the heap runs out showing it
  fresh();
  boot("");
  line("t = {} for i = 1, 2000 do t[i] = i end");
  lua_gc(board_L, LUA_GCCOLLECT, 0);
  heap_limit = heap_used + TOO_BIG_MARGIN;
  said = line("t");
  heap_limit = (size_t)-1;
  expect(strstr(said, "not enough memory") != NULL,
         "a result too big for the heap says so");
  said = line("print(6*7)");
  expect(armed && strstr(said, "42\r\n> ") != NULL,
         "... and the prompt comes back after it");
}

// Lines sent together run one after another: a line that sleeps ends
// before the next one starts, whether the next came with it or later
static void lines_in_turn(void) {
  static const char *const order[] = {
    "<sleep>", "</sleep>", "> print(2)\r\r\n2\r\n> ", NULL };
  static const char *const later[] = {
    "<sleep>", "</sleep>", "print(3)", "3\r\n> ", NULL };
  fresh();
  boot("");
  line("microbit.sleep(50)\rprint(2)");
  expect(in_order(out, order), "two lines sent at once run in turn");
  fresh();
  boot("");
  plan(type_and_post_the_port);
  line("microbit.sleep(50)");
  expect(in_order(out, later) && count(out, "print(3)") == 1,
         "the port's event waits out a sleep");
}

// A robot program sent at once, as slalom sends one: every move arrives
// and runs, though the first sleeps while the rest comes in
static void robot_file(void) {
  static const char MOVE[] = "robot_move(40, 40, 2)\r";
  char file[512] = "";
  char what[128];
  int i, moves[] = { 4, 10 };
  for (i = 0; i < 2; i++) {
    int k;
    fresh();
    boot("");
    file[0] = 0;
    for (k = 0; k < moves[i]; k++)
      strcat(file, MOVE);
    type_in(file);
    snprintf(what, sizeof what,
             "a %d-move file (%zu characters) sent at once runs whole",
             moves[i], strlen(file));
    expect(count(out, "<move 2000>") == moves[i] && lost == 0
           && strstr(out, "error") == NULL, what);
  }
  // 300 characters sent as the cable carries them, to an idle prompt
  fresh();
  boot("");
  {
    char text[400] = "x = \"";
    size_t n = strlen(text);
    memset(text + n, 'a', 290);
    strcpy(text + n + 290, "\"\r#x\r");
    send_paced(text, 12);
  }
  expect(lost == 0 && strstr(out, "=> 290\r\n> ") != NULL,
         "300 characters sent to the prompt arrive whole");
}

static void programs_alone(void) {
  // The usual shape: set on_event, then loop with microbit.sleep
  fresh();
  plan(press_a);
  plan(press_b_then_a);
  boot_program(
    "local got = {}\n"
    "function on_event(s, v) got[#got + 1] = s .. ':' .. v end\n"
    "for i = 1, 3 do microbit.sleep(100) end\n"
    "microbit.display.scroll(table.concat(got, ','))\n");
  expect(strstr(out, "<scroll 1:3,2:3,1:3>") != NULL,
         "a program that loops with sleep gets each press once, in order");

  {
    // A handler that sleeps nests no other handler
    static const char *const order[] = {
      "<scroll in1>", "<scroll out1>", "<scroll in2>", "<scroll out2>",
      "<booted>", NULL };
    fresh();
    plan(press_a);
    plan(press_b);
    boot_program(
      "function on_event(s, v)\n"
      "  microbit.display.scroll('in' .. s)\n"
      "  microbit.sleep(20)\n"
      "  microbit.display.scroll('out' .. s)\n"
      "end\n"
      "microbit.sleep(100)\n");
    expect(in_order(out, order) && count(out, "<scroll in") == 2,
           "a handler's own sleep handles nothing: each ends before the next");
  }
  {
    // The same once the program has returned: a handler started by an
    // event runs alone
    static const char *const order[] = {
      "<booted>", "<scroll in1>", "<scroll out1>", "<scroll in2>",
      "<scroll out2>", NULL };
    fresh();
    boot_program(
      "function on_event(s, v)\n"
      "  microbit.display.scroll('in' .. s)\n"
      "  microbit.sleep(20)\n"
      "  microbit.display.scroll('out' .. s)\n"
      "end\n");
    plan(press_b);
    press_a();
    expect(in_order(out, order), "... and after boot as well");
  }

  // Nothing but package, module and require is there before a program
  // requires it, and requiring touches no bus
  fresh();
  boot_alone(
    "if microbit ~= nil or tpbot ~= nil or robot_move ~= nil then\n"
    "  error('there at start')\n"
    "end\n"
    "local d = require('microbit.display')\n"
    "if microbit == nil or microbit.version ~= nil or microbit.display ~= d then\n"
    "  error('microbit.display')\n"
    "end\n"
    "local m = require('microbit')\n"
    "if m ~= microbit or microbit.version == nil then error('microbit') end\n"
    "if require('microbit') ~= m then error('once') end\n"
    "local t = require('tpbot')\n"
    "if t ~= tpbot or type(robot_move) ~= 'function'\n"
    "  or type(robot_info) ~= 'function' or type(turn) ~= 'function'\n"
    "  or type(straight) ~= 'function' then error('tpbot') end\n"
    "d.scroll('ok')\n");
  expect(strstr(out, "<scroll ok><booted>") != NULL && bus_writes == 0,
         "a program finds only require; require sets the globals it names, "
         "and tpbot's the robot commands, with nothing on the bus");

  // A metamethod of _G's that fails does not reach the dispatcher
  fresh();
  boot_program(
    "setmetatable(_G, { __index = function(t, k) error('undeclared ' .. k) "
    "end })\n"
    "microbit.display.scroll('set')\n");
  press_a();
  expect(strstr(out, "<scroll set><booted>") != NULL,
         "an _G whose __index fails leaves events harmless");

  // No on_event and no fallback: events go nowhere, and nothing breaks
  fresh();
  plan(press_a);
  boot_program("microbit.sleep(50) microbit.display.scroll('done')");
  press_b();
  expect(strstr(out, "<scroll done><booted>") != NULL
         && strstr(out, "<error") == NULL,
         "a program with no on_event runs on, its events dropped quietly");

  {
    // A program that stops on a mistake: the mistake is shown, then what
    // waited is handled by the on_event it had set
    static const char *const order[] = {
      "<scroll late1>", "<error Lua error!>", "<error ", "boom>",
      "<scroll late2>", "<booted>", NULL };
    fresh();
    plan(press_a);
    on_error = press_b;
    boot_program(
      "function on_event(s, v) microbit.display.scroll('late' .. s) end\n"
      "microbit.sleep(10)\n"
      "error('boom')\n");
    expect(in_order(out, order),
           "a press while the mistake is shown waits until it has been");
    press_a();
    expect(strstr(out, "<booted><scroll late1>") != NULL,
           "... and events are handled after it");
  }
}

static void round_two(void) {
  const char *said;
  {
    // A line whose port event waited behind another handler still has
    // safe points: B, pressed while it sleeps, is handled in the sleep
    static const char *const order[] = {
      "microbit.sleep(50)", "<sleep>", "<scroll B>", "</sleep>", NULL };
    fresh();
    boot("");
    line("microbit.handler[1] = function() microbit.sleep(20) end");
    plan(type_a_sleeping_line);
    plan(press_b);
    press_a();
    expect(in_order(out, order),
           "a line that waited behind a handler handles events as it sleeps");
  }
  // A sleep ends on time while a handler keeps raising events
  fresh();
  boot_program(
    "local n = 0\n"
    "function on_event(s, v) n = n + 1 microbit.post(1, 3) end\n"
    "microbit.sleep(0)\n"
    "microbit.post(1, 3)\n"
    "microbit.sleep(100)\n"
    "microbit.display.scroll(n > 0 and n < 50 and 'on time' or 'late')\n");
  expect(strstr(out, "<scroll on time>") != NULL,
         "a sleep ends on time while each handler raises another event");
  // What the firmware lists as noise never reaches a handler
  fresh();
  boot_program(
    "local shown = 0\n"
    "function on_event(s, v) if s == 7 then shown = shown + 1 end end\n"
    "microbit.display.scroll('x')\n"
    "microbit.sleep(50)\n"
    "microbit.display.scroll(shown == 0 and 'quiet' or 'noise')\n");
  expect(strstr(out, "<scroll quiet>") != NULL,
         "a scroll's end, and the port's own noise, reach no handler");
  // A handler's mistake is shown without holding up the program
  fresh();
  boot_program(
    "function on_event(s, v) error('oops') end\n"
    "microbit.sleep(0)\n"
    "microbit.post(1, 3)\n"
    "microbit.sleep(20)\n"
    "microbit.display.scroll('went on')\n");
  expect(strstr(out, "<error, going on ") != NULL
         && strstr(out, "<scroll went on>") != NULL,
         "a handler's mistake is shown without waiting for it");
  // sleep(0) lets other fibers run
  fresh();
  boot_program("microbit.sleep(0)");
  expect(pauses_all >= 1, "sleep(0) still pauses");
  {
    // A program's own on_event nests nothing, the port's event included
    static const char *const order[] = {
      "<scroll in12>", "<scroll out12>", "<scroll in2>", "<scroll out2>",
      NULL };
    fresh();
    boot_program(
      "function on_event(s, v)\n"
      "  microbit.display.scroll('in' .. s)\n"
      "  microbit.sleep(20)\n"
      "  microbit.display.scroll('out' .. s)\n"
      "end\n");
    plan(press_b);
    post(ID_SERIAL, HEAD_MATCH);
    expect(in_order(out, order),
           "a program's on_event given the port's event nests no other");
  }
  // The port's event, lost for want of a fiber while a handler ran, is
  // taken as waiting
  fresh();
  boot("");
  line("microbit.handler[1] = function() microbit.sleep(20) end");
  plan(type_and_miss_the_port);
  press_a();
  expect(strstr(out, "6*7\r\r\n=> 42") != NULL,
         "a port event that found no fiber is not lost");
  // A callable table whose __call is not a function falls back
  fresh();
  boot("");
  line("_G.on_event = setmetatable({}, { __call = 1 })");
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL,
         "an on_event with a __call that is not a function falls back");
  // An error from __gc does not keep the prompt away
  fresh();
  boot("");
  said = line("p = newproxy(true) getmetatable(p).__gc = "
              "function() error('gc') end p = nil");
  expect(strstr(said, "> ") != NULL && armed,
         "a __gc that raises an error leaves the prompt");
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL, "... and the REPL answering");
  // A character that comes as the echo goes out, before the port is armed
  // again, is read all the same
  fresh();
  boot("");
  send_trigger = "6";
  send_brings = "\r";
  type_in("6*7");
  expect(strstr(out, "=> 42\r\n> ") != NULL,
         "a character that came before the port was armed is read");
  // The port failing mid-line leaves it armed
  fresh();
  boot("");
  send_fails = 1;
  type_in("x");
  expect(armed, "the port is armed after a failure while reading it");
  type_in("\r");
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL, "... and the REPL answers");
}

// A handler loop that never ends would hang the run: it fails instead
static void too_long(int signal) {
  (void)signal;
  printf("FAIL the checks did not finish within a minute: an event loop "
         "that never ends?\n");
  exit(1);
}

// What the guard costs: nothing until an event has to wait for a program
// that has somewhere to send it
static void what_waiting_costs(void) {
  const char *said;
  char what[128];
  fresh();
  boot("");
  expect(board_allocs == 0, "the firmware's script boots with no line made");
  said = line("6*7");
  expect(board_allocs == 0 && strstr(said, "=> 42") != NULL,
         "... nor does a line typed at the prompt make one");
  plan(press_a);
  line("microbit.sleep(50)");
  snprintf(what, sizeof what,
           "the first press that waits makes the line: %zu bytes",
           board_bytes);
  expect(board_allocs == 1 && strstr(out, "<scroll A>") != NULL, what);
  plan(press_b);
  line("microbit.sleep(50)");
  expect(board_allocs == 1, "... and the next ones use it");

  fresh();
  plan(press_a);
  plan(press_b);
  boot_program("microbit.sleep(50) microbit.sleep(50) "
               "microbit.display.scroll('done')");
  expect(board_allocs == 0 && strstr(out, "<scroll done>") != NULL,
         "a program with no on_event makes no line for its presses");

  fresh();
  plan(press_a);
  plan(press_b);
  boot_program(
    "local got = 0\n"
    "function on_event(s, v) got = got + 1 end\n"
    "microbit.eventLine(0)\n"
    "microbit.sleep(50)\n"
    "microbit.display.scroll(got .. ' ' .. microbit.eventsDropped())\n");
  expect(strstr(out, "<scroll 0 2>") != NULL,
         "eventLine(0) drops every event that would have to wait");

  fresh();
  plan(press_a);
  plan(press_b);
  boot_program(
    "local got = 0\n"
    "function on_event(s, v) got = got + 1 end\n"
    "microbit.sleep(0)\n"
    "microbit.eventLine(1)\n"
    "microbit.sleep(50)\n"
    "microbit.display.scroll(got .. ' ' .. microbit.eventsDropped())\n");
  expect(strstr(out, "<scroll 2 0>") != NULL,
         "eventLine(1) keeps one event that waits at a time");
}

// The radio's inbox: made when the first link opens, not before
static void the_radio_inbox(void) {
  uint8_t body[RADIO_INBOX_BODY];
  int len = 0, before;
  fresh();
  before = board_allocs;
  expect(radio_inbox_full() && !radio_inbox_take(body, &len)
         && board_allocs == before,
         "before any link, the radio keeps nothing and made nothing");
  radio_inbox_open();
  expect(board_allocs == before + 1 && !radio_inbox_full(),
         "the first link makes the inbox");
  radio_inbox_put((const uint8_t *)"hi", 2);
  radio_inbox_put((const uint8_t *)"yo", 2);
  expect(radio_inbox_full(), "... which holds two pieces");
  expect(radio_inbox_take(body, &len) && len == 2 && body[0] == 'h'
         && radio_inbox_take(body, &len) && body[0] == 'y'
         && !radio_inbox_take(body, &len), "... given back in order");
  radio_inbox_open();
  expect(board_allocs == before + 1, "a link opened again makes no other");
}

// A C stack too full to go deeper is an error a pcall catches, and the
// prompt comes back; the host's limit stands in for the board's 7 KB
static void a_full_c_stack(void) {
  const char *said;
  fresh();
  boot("");
  host_cstack_limit = host_cstack_used() + 40000;
  line("function deeper(n) local ok, err = pcall(deeper, n + 1) "
       "if not ok and not stopped then stopped = n caught = err end end");
  line("deeper(1)");
  said = line("caught, stopped < 80");
  expect(strstr(said, "C stack overflow") != NULL
         && strstr(said, "true") != NULL,
         "pcalls nested too deep stop with \"C stack overflow\", "
         "before Lua's own limit of 200 C calls");
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL, "... and the REPL answers");
  said = line("local function f() return 1 + select(2, pcall(f)) end f()");
  expect(strstr(said, "> ") != NULL && armed,
         "recursion through pcall with no end stops the line, not the board");
  {
    // An expression nested too deep for the stack is a compile error
    char text[2048] = "x = ";
    int i;
    for (i = 0; i < 150; i++) strcat(text, "(");
    strcat(text, "1");
    for (i = 0; i < 150; i++) strcat(text, ")");
    host_cstack_limit = host_cstack_used() + 20000;
    said = line(text);
    expect(strstr(said, "C stack overflow") != NULL,
           "an expression nested too deep for the stack does not compile");
  }
  host_cstack_limit = (size_t)-1;
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL, "... and the REPL answers");
}

// A sleep deep in the stack runs no handlers: their events wait for the
// call to return
static void deep_sleeps(void) {
  static const char *const shallow[] = {
    "<sleep>", "<scroll A>", "</sleep>", NULL };
  static const char *const deep[] = {
    "<sleep>", "</sleep>", "> ", "<scroll A>", NULL };
  const char *said;
  fresh();
  boot("");
  stack_used_now = 4300;
  plan(press_a);
  said = line("microbit.sleep(50)");
  expect(in_order(said, shallow), "a sleep 4,300 bytes deep runs handlers");
  stack_used_now = 4400;
  plan(press_a);
  said = line("microbit.sleep(50)");
  expect(in_order(said, deep) && count(said, "<scroll A>") == 1,
         "a sleep 4,400 bytes deep leaves them for after the call");
}

// An uploaded program that sets on_event and then drives: a press during
// its first move reaches on_event when the move ends. And the guide's
// forwarding on_event, which passes what it does not handle to the REPL's:
// a command typed through it still handles presses while it sleeps.
static void codex_round_nine(void) {
  static const char *const forwarded[] = {
    "microbit.sleep(50)", "<sleep>", "<scroll hi>", "</sleep>", NULL };
  fresh();
  plan(press_a);
  boot_alone("require('microbit')\nrequire('microbit.display')\n"
             "require('tpbot')\n"
             "function on_event(s, v) microbit.display.scroll('got' .. s) end\n"
             "robot_move(40, 40, 2)\n");
  expect(strstr(out, "<move 2000><scroll got1>") != NULL
         && strstr(out, "<booted>") > strstr(out, "<scroll got1>"),
         "a press during an uploaded program's first move reaches its "
         "on_event when the move ends");
  fresh();
  boot("local original = on_event\n"
       "function on_event(s,v,t)\n"
       "if s == microbit.DEVICE_ID_BUTTON_A and "
       "v == microbit.DEVICE_BUTTON_EVT_CLICK then\n"
       "microbit.display.scroll('hi')\n"
       "else original(s,v,t) end end\n");
  plan(press_a);
  line("microbit.sleep(50)");
  expect(in_order(out, forwarded),
         "a command typed through a forwarding on_event handles a press "
         "while it sleeps");
}

int main(int argc, char **argv) {
  host_cstack_start();
  signal(SIGALRM, too_long);
  alarm(60);
  if (argc != 2) {
    fprintf(stderr, "usage: %s source/lua-script.lua\n", argv[0]);
    return 2;
  }
  script = slurp(argv[1], &script_len);
  the_repl();
  events_and_the_repl();
  on_event_and_print();
  globals_taken_away();
  the_repl_always_comes_back();
  lines_in_turn();
  robot_file();
  round_two();
  what_waiting_costs();
  the_radio_inbox();
  a_full_c_stack();
  deep_sleeps();
  codex_round_nine();
  programs_alone();
  printf("%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
