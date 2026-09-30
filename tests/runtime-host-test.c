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

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lua-events.h"
#include "lua-print.h"
#include "tpbot.h"

void lua_strip_debug(lua_State *L);

enum {
  ID_BUTTON_A = 1, ID_BUTTON_B = 2, ID_BUTTON_AB = 3, ID_RADIO = 9,
  ID_SERIAL = 12, HEAD_MATCH = 2, CLICK = 3, LONG_CLICK = 4
};

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

void lua_events_show_error(const char *message) {
  Action then = on_error;
  on_error = NULL;
  note("<error ");
  note(message);
  note(">");
  if (then)
    then();
}

uint32_t lua_events_now(void) {
  return clock_ms;
}

void lua_events_pause(uint32_t ms) {
  clock_ms += ms;
  if (pauses_done < pauses_planned)
    pauses[pauses_done++]();
}

void lua_print_out(const char *text, size_t length) {
  char piece[1024];
  if (length >= sizeof piece) length = sizeof piece - 1;
  memcpy(piece, text, length);
  piece[length] = 0;
  note(piece);
}

// A robot that takes every command; robot_move's sleep is the firmware's,
// which lets other fibers run and handles no events.
int tpbot_i2c_write(int address, const char *data, size_t length) {
  (void)address; (void)data; (void)length;
  return 0;
}
void tpbot_sleep(uint32_t ms) {
  char text[32];
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
  armed = 1;
  return 0;
}

static int l_scroll(lua_State *L) {
  note("<scroll ");
  note(luaL_checkstring(L, 1));
  note(">");
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

static void set(lua_State *L, const char *name, lua_CFunction f) {
  lua_pushcfunction(L, f);
  lua_setfield(L, -2, name);
}

static void constant(lua_State *L, const char *name, int value) {
  lua_pushinteger(L, value);
  lua_setfield(L, -2, name);
}

static void board(lua_State *L) {
  lua_newtable(L);
  set(L, "version", l_version);
  set(L, "friendlyName", l_name);
  set(L, "sleep", l_sleep);
  set(L, "eventsDropped", lua_events_dropped);
  set(L, "eventFallback", lua_events_fallback);
  constant(L, "DEVICE_ID_BUTTON_A", ID_BUTTON_A);
  constant(L, "DEVICE_ID_BUTTON_B", ID_BUTTON_B);
  constant(L, "DEVICE_ID_BUTTON_AB", ID_BUTTON_AB);
  constant(L, "DEVICE_ID_RADIO", ID_RADIO);
  constant(L, "DEVICE_ID_SERIAL", ID_SERIAL);
  constant(L, "CODAL_SERIAL_EVT_HEAD_MATCH", HEAD_MATCH);
  constant(L, "DEVICE_BUTTON_EVT_CLICK", CLICK);
  constant(L, "DEVICE_BUTTON_EVT_LONG_CLICK", LONG_CLICK);
  lua_newtable(L);
  set(L, "setVolume", l_nothing);
  set(L, "express", l_nothing);
  lua_setfield(L, -2, "audio");
  lua_newtable(L);
  set(L, "animate", l_nothing);
  set(L, "scrollAsync", l_nothing);
  set(L, "scroll", l_scroll);
  lua_setfield(L, -2, "display");
  lua_newtable(L);
  set(L, "send", l_send);
  set(L, "getCharAsync", l_get_char);
  set(L, "eventAfterAsync", l_arm);
  lua_setfield(L, -2, "serial");
  lua_newtable(L);
  set(L, "enable", l_nothing);
  set(L, "listen", l_nothing);
  set(L, "connect", l_connect);
  set(L, "answered", l_nothing);
  set(L, "tx", l_tx);
  set(L, "rx", l_rx);
  lua_setfield(L, -2, "radio");
  lua_setglobal(L, "microbit");
}

// What the next pauses do, from the next one on
static void plan(Action a) {
  pauses[pauses_planned++] = a;
}

static void fresh(void) {
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
  board(L);
  tpbot_register_globals(L);
  lua_print_open(L);
  lua_events_open(L, ID_SERIAL, HEAD_MATCH);
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
  text = malloc(*length);
  if (fread(text, 1, *length, f) != *length) { perror(path); exit(2); }
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

static void globals_taken_away(void) {
  const char *said;
  char what[256], take[128];
  int i;
  for (i = 0; GLOBALS[i]; i++) {
    fresh();
    boot("");
    snprintf(take, sizeof take, "_G.%s = nil", GLOBALS[i]);
    line(take);
    said = line("print(6*7)");
    snprintf(what, sizeof what, "print answers after %s", take);
    expect(!strcmp(GLOBALS[i], "print")
           || strstr(said, "42\r\n> ") != NULL, what);
    still_answers(take);
  }
  for (i = 0; LIBRARY_FIELDS[i]; i++) {
    fresh();
    boot("");
    snprintf(take, sizeof take, "%s = nil", LIBRARY_FIELDS[i]);
    line(take);
    still_answers(take);
  }
  for (i = 0; GLOBALS[i]; i++) {
    fresh();
    snprintf(take, sizeof take, "_G.%s = nil", GLOBALS[i]);
    boot(take);
    said = line("6*7");
    snprintf(what, sizeof what, "the REPL answers after a program's %s",
             take);
    expect(strstr(said, "=> 42\r\n> ") != NULL, what);
  }
}

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
  line("t = {} for i = 1, 500 do t[i] = i end");
  lua_gc(board_L, LUA_GCCOLLECT, 0);
  heap_limit = heap_used + 3000;
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
  boot_alone(
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
    boot_alone(
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
    boot_alone(
      "function on_event(s, v)\n"
      "  microbit.display.scroll('in' .. s)\n"
      "  microbit.sleep(20)\n"
      "  microbit.display.scroll('out' .. s)\n"
      "end\n");
    plan(press_b);
    press_a();
    expect(in_order(out, order), "... and after boot as well");
  }

  // print goes to the port, a line at a time
  fresh();
  boot_alone("print(1, 'a') print('x\\ny') print()");
  expect(strstr(out, "1\ta\r\nx\r\ny\r\n\r\n<booted>") != NULL,
         "a program's print writes to the port, lines ended by \\r\\n");

  // A metamethod of _G's that fails does not reach the dispatcher
  fresh();
  boot_alone(
    "setmetatable(_G, { __index = function(t, k) error('undeclared ' .. k) "
    "end })\n"
    "microbit.display.scroll('set')\n");
  press_a();
  expect(strstr(out, "<scroll set><booted>") != NULL,
         "an _G whose __index fails leaves events harmless");

  // No on_event and no fallback: events go nowhere, and nothing breaks
  fresh();
  plan(press_a);
  boot_alone("microbit.sleep(50) microbit.display.scroll('done')");
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
    boot_alone(
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

int main(int argc, char **argv) {
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
  programs_alone();
  printf("%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
