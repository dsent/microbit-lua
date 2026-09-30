// -*- mode: c; indent-tabs-mode: nil; -*-
//
// Host test for source/lua-script.lua and source/lua-events.c: runs the
// script, loaded and stripped as the firmware loads it, in this firmware's
// Lua built for the host, over a stand-in board, with events handed in
// through the firmware's own dispatcher. It types at the REPL, presses
// buttons while Lua sleeps, and takes the globals the REPL uses away,
// and checks what comes out of the port.
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
// what waits to be read from the port, and whether the port is armed.
static char out[65536];
static char typed[4096];
static size_t typed_at, typed_len;
static int armed;
static lua_State *board_L;

// What the stand-in microbit.sleep does, once, when next called.
static void (*during_sleep)(void);

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
  note("<error ");
  note(message);
  note(">");
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

static int l_sleep(lua_State *L) {
  void (*then)(void) = during_sleep;
  (void)L;
  note("<sleep>");
  during_sleep = NULL;
  if (then)
    then();
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

static char *script;
static size_t script_len;

// Boot as the firmware does, with program put where the IDE puts a file:
// before the script's last serial_session.prompt().
static void boot(const char *program, void (*action)(void)) {
  static const char PROMPT[] = "\nserial_session.prompt()";
  char *text, *at = NULL, *p = script;
  size_t head, n = strlen(program);
  lua_State *L;

  while ((p = strstr(p, PROMPT)) != NULL) at = p++;
  if (!at) { fprintf(stderr, "no prompt line\n"); exit(2); }
  head = (size_t)(at - script) + 1;
  text = malloc(script_len + n + 2);
  memcpy(text, script, head);
  memcpy(text + head, program, n);
  text[head + n] = '\n';
  memcpy(text + head + n + 1, script + head, script_len - head);

  out[0] = 0;
  typed_at = typed_len = 0;
  armed = 0;
  during_sleep = action;
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
  if (luaL_loadbuffer(L, text, script_len + n + 1, "embedded")) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    exit(2);
  }
  free(text);
  lua_strip_debug(L);
  lua_call_begin();
  if (lua_pcall(L, 0, 0, 0))
    note("<boot error>");
  note("<boot returned>");
  lua_call_end();
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

// Type a line at the prompt; what came out since
static const char *line(const char *text) {
  static char said[65536];
  size_t before = strlen(out);
  type_in(text);
  type_in("\r");
  snprintf(said, sizeof said, "%s", out + before);
  return said;
}

static void press_a_three_times(void) {
  post(ID_BUTTON_A, CLICK);
  post(ID_BUTTON_A, CLICK);
  post(ID_BUTTON_A, CLICK);
}

static void type_during_boot(void) {
  post(ID_BUTTON_A, CLICK);
  type_in("print(6*7)\r");
}

// Twenty presses while Lua sleeps, more than the line holds, then the
// port's event with a line typed
static void flood(void) {
  int i;
  for (i = 0; i < 20; i++)
    post(ID_BUTTON_B, CLICK);
  memcpy(typed + typed_len, "6*7\r", 4);
  typed_len += 4;
  post(ID_SERIAL, HEAD_MATCH);
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

static const char *after(const char *text, const char *mark) {
  const char *at = strstr(text, mark);
  return at ? at + strlen(mark) : "";
}

int main(int argc, char **argv) {
  const char *said;
  char what[256], take[128];
  int i;
  if (argc != 2) {
    fprintf(stderr, "usage: %s source/lua-script.lua\n", argv[0]);
    return 2;
  }
  script = slurp(argv[1], &script_len);

  boot("", NULL);
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

  // A program that sleeps at boot, as upload() runs one: a button pressed
  // meanwhile is handled once, after the script; a line typed meanwhile
  // reaches the REPL.
  boot("microbit.sleep(100)", type_during_boot);
  expect(count(out, "<scroll A>") == 1, "a press during boot is handled once");
  expect(strstr(after(out, "<boot returned>"), "<scroll A>") != NULL,
         "... after the script has returned");
  expect(strstr(out, "> print(6*7)\r\r\n42\r\n> ") != NULL,
         "a line typed during boot is run at the prompt");

  // A command that sleeps: presses meanwhile wait for it
  boot("", NULL);
  during_sleep = press_a_three_times;
  said = line("microbit.sleep(5)");
  expect(count(said, "<scroll A>") == 3, "three presses during a command");
  expect(strstr(said, "</sleep><scroll") == NULL
         && strstr(after(said, "</sleep>"), "> <scroll A>") != NULL,
         "... each handled after the command and its prompt");

  // More presses than the line holds: the extra ones are dropped and
  // counted, and the port's event is not among them
  boot("", NULL);
  during_sleep = flood;
  said = line("microbit.sleep(5)");
  expect(count(said, "<scroll B>") == 16, "sixteen of twenty presses kept");
  expect(strstr(said, "=> 42") != NULL, "the line typed meanwhile is run");
  said = line("microbit.eventsDropped()");
  expect(strstr(said, "=> 4\r\n") != NULL, "four counted as dropped");

  // A program's on_event: a function replaces the firmware's; anything
  // else leaves events to the firmware's
  boot("", NULL);
  line("_G.on_event = 1");
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL, "on_event = 1 leaves the REPL");
  boot("", NULL);
  line("_G.on_event = nil");
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL, "on_event = nil leaves the REPL");
  boot("on_event = 'x'", NULL);
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL,
         "on_event = 'x' from a program at boot leaves the REPL");
  boot("function on_event(s, v) microbit.display.scroll('mine') end", NULL);
  line("6*7");
  expect(count(out, "<scroll mine>") == 1
         && strstr(out, "=> 42") == NULL,
         "a program's own on_event takes every event");

  // A program's own print reaches its own code; the REPL keeps its own
  boot("", NULL);
  line("function print() io.write('mine\\n') end");
  said = line("print(1)");
  expect(strstr(said, "mine\r\n> ") != NULL, "a print of one's own is used");
  said = line("1+1");
  expect(strstr(said, "=> 2\r\n> ") != NULL, "... and the REPL still answers");

  for (i = 0; GLOBALS[i]; i++) {
    boot("", NULL);
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
    boot("", NULL);
    snprintf(take, sizeof take, "%s = nil", LIBRARY_FIELDS[i]);
    line(take);
    said = line("'hi', {a = 1}");
    snprintf(what, sizeof what, "the REPL answers after %s", take);
    expect(strstr(said, "=> \"hi\"\t{a = 1}\r\n> ") != NULL, what);
  }
  // The same from a program at boot, as upload() runs one
  for (i = 0; GLOBALS[i]; i++) {
    snprintf(take, sizeof take, "_G.%s = nil", GLOBALS[i]);
    boot(take, NULL);
    said = line("6*7");
    snprintf(what, sizeof what, "the REPL answers after a program's %s", take);
    expect(strstr(said, "=> 42\r\n> ") != NULL, what);
  }

  printf("%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
