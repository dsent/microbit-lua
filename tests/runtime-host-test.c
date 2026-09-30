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

// Characters arriving at the port: the armed event fires on the first.
static void type_in(const char *text) {
  size_t n = strlen(text);
  memcpy(typed + typed_len, text, n);
  typed_len += n;
  if (armed) {
    armed = 0;
    post(ID_SERIAL, HEAD_MATCH);
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

int tpbot_i2c_write(int address, const char *data, size_t length) {
  (void)address; (void)data; (void)length;
  return -1;
}
void tpbot_sleep(uint32_t ms) { (void)ms; }
int tpbot_echo_us(void) { return -1; }

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
  set(L, "connect", l_nothing);
  set(L, "answered", l_nothing);
  set(L, "tx", l_nothing);
  set(L, "rx", l_nothing);
  lua_setfield(L, -2, "radio");
  lua_setglobal(L, "microbit");
}

// What the next pauses do, from the next one on
static void plan(Action a) {
  pauses[pauses_planned++] = a;
}

static void fresh(void) {
  out[0] = 0;
  typed_at = typed_len = 0;
  armed = 0;
  clock_ms = 0;
  pauses_planned = pauses_done = 0;
  on_error = NULL;
}

// Boot the board with text as its script, as the firmware does
static void boot_script(const char *text, size_t length) {
  lua_State *L;
  if (board_L) lua_close(board_L);
  board_L = L = luaL_newstate();
  luaopen_base(L);
  luaopen_table(L);
  luaopen_string(L);
  luaopen_math(L);
  lua_settop(L, 0);
  board(L);
  tpbot_register_globals(L);
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
  "microbit.serial.eventAfterAsync", "microbit.handler", NULL
};

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
    said = line("'hi', {a = 1}");
    snprintf(what, sizeof what, "the REPL answers after %s", take);
    expect(strstr(said, "=> \"hi\"\t{a = 1}\r\n> ") != NULL, what);
    said = line("print(6*7)");
    snprintf(what, sizeof what, "... and print after %s", take);
    expect(!strcmp(GLOBALS[i], "print")
           || strstr(said, "42\r\n> ") != NULL, what);
  }
  for (i = 0; LIBRARY_FIELDS[i]; i++) {
    fresh();
    boot("");
    snprintf(take, sizeof take, "%s = nil", LIBRARY_FIELDS[i]);
    line(take);
    said = line("'hi', {a = 1}");
    snprintf(what, sizeof what, "the REPL answers after %s", take);
    expect(strstr(said, "=> \"hi\"\t{a = 1}\r\n> ") != NULL, what);
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
  programs_alone();
  printf("%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
