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
#include "lobject.h"
#include "lua-events.h"
#include "lua-modules.h"
#include "board-alloc.h"
#include "radio-link.h"
#include "host-cstack.h"
#include "tpbot.h"

#include "lua-strip-debug.h"

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

static int board_refuses;

void *board_alloc(size_t size) {
  if (board_refuses)
    return NULL;
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
// What arrives at the port the moment a read finds it empty, once
static const char *empty_brings;
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

// An event that found no fiber to carry it
static void miss(int source, int value) {
  LuaEvent e = { (uint16_t)source, (uint16_t)value, 0 };
  lua_events_missed(e);
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

void lua_events_port_error(const char *what, const char *message) {
  note("\r\n");
  note(what);
  note(message);
  note("\r\n");
}

bool lua_events_arm_port(void) {
  armed = 1;
  return typed_at < typed_len;
}

// The stack in use a safe point sees: 0, or what a test says
static uint32_t stack_used_now;

uint32_t lua_events_stack_used(void) {
  return stack_used_now;
}

// The stack region's size: the board's 8 KB, or what a test says
static uint32_t stack_region = 8192;

uint32_t lua_events_stack_region(void) {
  return stack_region;
}

uint32_t lua_events_now(void) {
  return clock_ms;
}

static int pauses_all;

static void others_run(void);

// What happens at every pause, after what was planned for it
static Action every_pause;

void lua_events_pause(uint32_t ms) {
  pauses_all++;
  clock_ms += ms;
  others_run();
  if (every_pause)
    every_pause();
  if (pauses_done < pauses_planned)
    pauses[pauses_done++]();
}

// Lua's heap, below: how much it holds, and how much it may
static size_t heap_used, heap_limit;

// A robot that takes every command, unless a test says which write fails,
// and whether the heap runs out with it; what went to the bus, frame by
// frame. robot_move's sleep is the firmware's, which lets other fibers run
// and handles no events.
static int bus_writes, bus_fails_at, bus_starves;
static char bus_log[1024];

int tpbot_i2c_write(int address, const char *data, size_t length) {
  size_t i, n;
  (void)address;
  bus_writes++;
  for (i = 0; i < length; i++) {
    n = strlen(bus_log);
    snprintf(bus_log + n, sizeof bus_log - n, i ? " %02X" : "%02X",
             (unsigned char)data[i]);
  }
  n = strlen(bus_log);
  snprintf(bus_log + n, sizeof bus_log - n, "|");
  if (bus_writes != bus_fails_at)
    return 0;
  if (bus_starves)
    heap_limit = heap_used;
  return -1;
}
// robot_move's sleep, as the firmware binds it; and what a test has
// happen while the robot drives
static Action on_move;

void tpbot_sleep(uint32_t ms) {
  char text[32];
  Action then = on_move;
  on_move = NULL;
  lua_events_before_wait();
  snprintf(text, sizeof text, "<move %lu>", (unsigned long)ms);
  note(text);
  lua_events_pause(ms);
  if (then)
    then();
}
int tpbot_echo_us(void) { return -1; }

// A real link, source/radio-link.c, once a test says so: this board's end,
// which its Lua reaches through the radio module below, and the other
// board's, "gigat", in C alone, which the test drives. Each end hears what
// the other sends in a queue of 4, as CODAL keeps no more: a datagram that
// finds its queue full is lost, and one that does not raises the radio's
// event on its board. The fibers carrying this board's events run whenever
// its Lua lets other fibers run, as on the board: each answers the link,
// then hands the event to the dispatcher. The other board's run then too.
static int real_link;

typedef struct {
  uint8_t frame[4][RADIO_AIR_MAX];
  int len[4], first, count;
  int events;                   // datagram events no fiber has carried yet
} Air;

static const RadioAir test_air;
static RadioLink board_link = { .air = &test_air }, far_link = { .air = &test_air };
static Air board_air, far_air;  // what each end hears

// The other board: gone from the air; listening for a call; or busy
// running a command, its Lua taking nothing from the link; answering each
// piece it takes with "=> " and the piece; with its answers lost on the
// way; and whether another pair talks on the air after each of its turns.
// What it took otherwise, how many of its answers failed, and how many
// answers this one sent.
static int far_gone, far_listens, far_busy, far_sending, neighbours;
static int far_echoes, far_echo_fails, far_unheard;

// Another pair, whose link's number shares this one's low byte, answering
// whatever this board sends; and what happens at this board's next wait
// on the radio
static int alike_answers;
static Action on_air;
static char far_heard[1024];
static int board_acks;

// The frames' kinds and the protocol's version, as the link has them
enum {
  KIND_HELLO = 0xA1, KIND_DATA = 0xA3, KIND_ACK = 0xA4, KIND_FIRST = 0xA5,
  VERSION = 3
};

// The link's number this pair draws, and another pair's that shares its
// low byte, the whole number a link had before
#define LINK 0x5EED0007u
#define LINK_ALIKE 0x6A110007u

static void air_put(Air *a, const uint8_t *frame, int len) {
  int at;
  if (a->count == 4)
    return;
  at = (a->first + a->count) % 4;
  memcpy(a->frame[at], frame, len);
  a->len[at] = len;
  a->count++;
  a->events++;
}

// A frame as the link puts one, put on the air: kind, link, number, body
static void air_frame(Air *a, uint8_t kind, uint32_t link, uint16_t num,
                      const char *body, int len) {
  uint8_t f[RADIO_FRAME] = {
    kind, (uint8_t)link, (uint8_t)(link >> 8), (uint8_t)(link >> 16),
    (uint8_t)(link >> 24), (uint8_t)num, (uint8_t)(num >> 8)
  };
  memcpy(f + RADIO_HEAD, body, len);
  air_put(a, f, RADIO_HEAD + len);
}

static void air_send(RadioLink *r, const uint8_t *frame, int len) {
  if (r == &board_link) {
    if (frame[0] == KIND_ACK)
      board_acks++;
    if (!far_gone)
      air_put(&far_air, frame, len);
  } else if (!(far_unheard && frame[0] == KIND_ACK)) {
    air_put(&board_air, frame, len);
  }
}

static int air_recv(RadioLink *r, uint8_t frame[RADIO_AIR_MAX]) {
  Air *a = r == &board_link ? &board_air : &far_air;
  int len;
  if (a->count == 0)
    return -1;
  len = a->len[a->first];
  memcpy(frame, a->frame[a->first], len);
  a->first = (a->first + 1) % 4;
  a->count--;
  return len;
}

static uint32_t air_now(RadioLink *r) {
  (void)r;
  return clock_ms;
}

// This board's fibers run while either board waits; the other board's own
// run from this board's waits, where the test drives it
static void board_fibers(void);

static void air_pause(RadioLink *r, uint32_t ms) {
  clock_ms += ms;
  if (r == &far_link) {
    board_fibers();
    return;
  }
  if (on_air) {
    Action then = on_air;
    on_air = NULL;
    then();
  }
  if (alike_answers)
    air_frame(&board_air, KIND_ACK, LINK_ALIKE, board_link.out, "", 0);
  others_run();
}

static const RadioAir test_air = { air_send, air_recv, air_now, air_pause };

// This board's fibers for the radio's events, as the firmware's carries one
static void board_fibers(void) {
  while (board_air.events > 0) {
    board_air.events--;
    if (radio_link_heard(&board_link))
      post(ID_RADIO, 1);
  }
}

// The other board: answers a call when it listens, answers the link from
// its fibers, and takes what came when it is not busy
static void far_runs(void) {
  uint8_t body[RADIO_BODY];
  int len;
  bool starts;
  if (far_gone)
    return;
  if (far_listens && far_link.link == 0)
    radio_link_called(&far_link, "gigat", NULL);
  while (far_air.events > 0) {
    far_air.events--;
    radio_link_heard(&far_link);
  }
  if (neighbours)
    air_frame(&board_air, KIND_DATA, LINK_ALIKE, 1, "hi", 2);
  while (!far_busy && !far_sending && radio_link_rx(&far_link, body, &len,
                                                          &starts)) {
    size_t n = strlen(far_heard);
    snprintf(far_heard + n, sizeof far_heard - n, "%.*s", len, (char *)body);
    if (far_echoes) {
      char answer[RADIO_BODY + 8];
      snprintf(answer, sizeof answer, "=> %.*s\n", len - 1, (char *)body);
      far_sending = 1;
      if (!radio_link_tx(&far_link, answer, strlen(answer)))
        far_echo_fails++;
      far_sending = 0;
    }
  }
}

static int others_running;

static void others_run(void) {
  if (others_running)
    return;
  others_running = 1;
  far_runs();
  board_fibers();
  others_running = 0;
}

// The other board sends a line
static int far_says(const char *text) {
  int ok;
  far_sending = 1;
  ok = radio_link_tx(&far_link, text, strlen(text));
  far_sending = 0;
  return ok;
}

// The other board calls this one, and opens its end as the answer will
static void far_calls(void) {
  char hello[RADIO_NAME * 2 + 1] = "zezopgigat";
  hello[RADIO_NAME * 2] = VERSION;
  air_frame(&board_air, KIND_HELLO, LINK, 0, hello, sizeof hello);
  radio_link_open(&far_link, LINK, "zezop");
}

// This board's radio bindings, as the firmware's call the link
static int link_listen(lua_State *L) {
  const char *from = lua_isnoneornil(L, 1) ? NULL : luaL_checkstring(L, 1);
  lua_events_before_wait();
  while (!radio_link_called(&board_link, "zezop", from))
    air_pause(&board_link, 1);
  lua_pushstring(L, board_link.peer);
  return 1;
}

static int looks_left;

static int link_answered(lua_State *L) {
  const char *from = lua_isnoneornil(L, 1) ? NULL : luaL_checkstring(L, 1);
  if (--looks_left < 0)
    return luaL_error(L, "hung up");
  if (radio_link_called(&board_link, "zezop", from))
    lua_pushstring(L, board_link.peer);
  else
    lua_pushnil(L);
  return 1;
}

static int link_connect(lua_State *L) {
  const char *them = luaL_checkstring(L, 1);
  lua_events_before_wait();
  lua_pushboolean(L, radio_link_call(&board_link, them, "zezop", LINK,
                                     (uint32_t)luaL_optint(L, 2, 5000)));
  return 1;
}

static int link_tx(lua_State *L) {
  size_t len;
  const char *msg = luaL_checklstring(L, 1, &len);
  lua_events_before_wait();
  lua_pushboolean(L, radio_link_tx(&board_link, msg, len));
  return 1;
}

static int board_took;

static int link_rx(lua_State *L) {
  uint8_t body[RADIO_BODY];
  int len;
  bool starts;
  if (!radio_link_rx(&board_link, body, &len, &starts)) {
    lua_pushnil(L);
    return 1;
  }
  board_took++;
  lua_pushlstring(L, (const char *)body, len);
  lua_pushboolean(L, starts);
  return 2;
}

// Both ends as a new test finds them: no link, nothing on the air
static void no_link(void) {
  board_free(board_link.inbox);
  board_free(far_link.inbox);
  memset(&board_link, 0, sizeof board_link);
  memset(&far_link, 0, sizeof far_link);
  board_link.air = far_link.air = &test_air;
  memset(&board_air, 0, sizeof board_air);
  memset(&far_air, 0, sizeof far_air);
  real_link = far_gone = far_listens = far_busy = far_sending = 0;
  neighbours = far_echoes = far_echo_fails = far_unheard = 0;
  alike_answers = 0;
  on_air = NULL;
  empty_brings = NULL;
  far_heard[0] = 0;
  board_acks = 0;
  looks_left = 20;
}

// A radio link whose far end is a board that answers each line with
// "=> " and the line: what tx sends comes back through rx, with the
// radio's event.
static char far_end[1024];
static int linked;

// Or, once listen() is called, a board that calls this one: it types what
// the test gives it, and hears what this one says. It hangs up after a few
// looks, which ends listen() with an error.
static const char *caller_types;
static char caller_hears[1024];
static int listening, looks;

static int l_listen(lua_State *L) {
  if (real_link)
    return link_listen(L);
  listening = 1;
  return 0;
}

static int l_answered(lua_State *L) {
  if (real_link)
    return link_answered(L);
  if (listening && ++looks > 3)
    return luaL_error(L, "hung up");
  lua_pushboolean(L, 0);
  return 1;
}

static int l_connect(lua_State *L) {
  if (real_link)
    return link_connect(L);
  linked = 1;
  lua_pushboolean(L, 1);
  return 1;
}

static int l_tx(lua_State *L) {
  const char *text;
  size_t n = strlen(far_end);
  if (real_link)
    return link_tx(L);
  text = luaL_checkstring(L, 1);
  if (listening) {
    n = strlen(caller_hears);
    snprintf(caller_hears + n, sizeof caller_hears - n, "%s", text);
    lua_pushboolean(L, 1);
    return 1;
  }
  snprintf(far_end + n, sizeof far_end - n, "=> %s\n", text);
  lua_pushboolean(L, 1);
  post(ID_RADIO, 1);
  return 1;
}

static int l_rx(lua_State *L) {
  if (real_link)
    return link_rx(L);
  if (listening && caller_types) {
    lua_pushstring(L, caller_types);
    caller_types = NULL;
    return 1;
  }
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

static int sends;

static int l_send(lua_State *L) {
  const char *text = luaL_checkstring(L, 1);
  sends++;
  if (send_fails) {
    send_fails = 0;
    return luaL_error(L, "the port failed");
  }
  note(text);
  if (send_trigger && strncmp(text, send_trigger, strlen(send_trigger)) == 0) {
    size_t n = strlen(send_brings);
    memcpy(typed + typed_len, send_brings, n);
    typed_len += n;
    send_trigger = NULL;
  }
  return 0;
}


static int l_get_char(lua_State *L) {
  if (typed_at == typed_len) {
    const char *brings = empty_brings;
    empty_brings = NULL;
    if (brings)
      type_in(brings);
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
  {"enable", l_nothing, 0}, {"listen", l_listen, 0},
  {"connect", l_connect, 0}, {"answered", l_answered, 0},
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
  stack_region = 8192;
  board_allocs = 0;
  board_bytes = 0;
  board_refuses = 0;
  pauses_all = 0;
  send_trigger = NULL;
  send_fails = 0;
  posts_left = 1000;
  bus_writes = bus_fails_at = bus_starves = 0;
  bus_log[0] = 0;
  lost = 0;
  linked = 0;
  far_end[0] = 0;
  no_link();
  on_move = NULL;
  every_pause = NULL;
  caller_types = NULL;
  caller_hears[0] = 0;
  listening = looks = 0;
  out[0] = 0;
  typed_at = typed_len = 0;
  armed = 0;
  clock_ms = 0;
  pauses_planned = pauses_done = 0;
  on_error = NULL;
}

// The heap, for the tests that run it out
static size_t heap_limit = (size_t)-1;

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

// A script at boot that does not load stops the tests, but where a test
// looks for that
static int boot_may_fail;

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
  if (!lua_events_boot_program(L, text, length, NULL) && !boot_may_fail) {
    fprintf(stderr, "the script at boot did not load:%s\n", out);
    exit(2);
  }
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
  miss(ID_SERIAL, HEAD_MATCH);
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
  "string.format", "string.find", "string.gmatch", "string.gsub",
  "string.match",
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

// The REPL keeps loadstring, setfenv, pcall, gsub, sub and concat of its
// own: a global it uses, taken away, can stop what a line shows, but the
// prompt comes back and the port is armed, so the global can be put back
// from the prompt. print is the REPL's own too.
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
  // A global the REPL compiles with, taken away and put back from the
  // prompt: the line that puts it back runs
  for (i = 0; i < 2; i++) {
    static const char *const COMPILER[] = { "loadstring", "setfenv" };
    fresh();
    boot("");
    snprintf(take, sizeof take, "saved = %s _G.%s = nil", COMPILER[i],
             COMPILER[i]);
    line(take);
    snprintf(take, sizeof take, "_G.%s = saved", COMPILER[i]);
    line(take);
    said = line("6*7");
    snprintf(what, sizeof what, "_G.%s taken away is put back from the "
             "prompt, and the REPL answers", COMPILER[i]);
    expect(strstr(said, "=> 42\r\n> ") != NULL, what);
  }
  // The port's own functions, put out of reach: the REPL keeps its own
  for (i = 0; i < 2; i++) {
    static const char *const PORT[] = {
      "microbit.eventRepl", "microbit.serial.eventAfterAsync" };
    fresh();
    boot("");
    snprintf(take, sizeof take, "%s = false", PORT[i]);
    line(take);
    line("6*8");
    said = line("6*7");
    snprintf(what, sizeof what, "the REPL answers after %s", take);
    expect(armed && strstr(said, "=> 42\r\n> ") != NULL, what);
  }
  // An on_event that fails on the port's event, before it passes it on to
  // the REPL: the port is armed all the same
  fresh();
  boot("");
  line("local original = on_event _G.on_event = function(s, v, t) "
       "if s == 12 and not failed then failed = true error('once') end "
       "original(s, v, t) end");
  type_in("6*8\r");
  expect(armed && strstr(out, "<error, going on ") != NULL
         && strstr(out, "=> 48\r\n> ") != NULL,
         "the port is armed after an on_event fails on its event, and what "
         "waits there is read");
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL, "... and the REPL answers");
  // An on_event that always fails on the port's event: it runs once more
  // for what waits, and no more
  fresh();
  boot("");
  line("_G.on_event = function(s, v, t) if s == 12 then error('no') end end");
  type_in("6*8\r");
  expect(count(out, "<error, going on ") == 2 && armed,
         "... and an on_event that always fails on it runs once more, "
         "not for ever");
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
  // A result too big to show: the heap runs out showing it. The host's
  // allocator refuses; on the board CODAL stops with 020 first
  fresh();
  boot("");
  line("t = {} for i = 1, 2000 do t[i] = i end");
  lua_gc(board_L, LUA_GCCOLLECT, 0);
  heap_limit = heap_used + TOO_BIG_MARGIN;
  said = line("t");
  heap_limit = (size_t)-1;
  expect(strstr(said, "not enough memory") != NULL,
         "a result too big for the host's heap says so");
  said = line("print(6*7)");
  expect(armed && strstr(said, "42\r\n> ") != NULL,
         "... and the prompt comes back after it");
}

// What is typed at once is echoed in one send, what the REPL says in
// sends of 64 characters, the bytes as they were
static void echo_in_pieces(void) {
  const char *said;
  int before;
  fresh();
  boot("");
  before = sends;
  type_in("x = 'abcdefghijklmnop'\r");
  expect(strstr(out, "x = 'abcdefghijklmnop'\r\r\n> ") != NULL
         && sends - before == 3,
         "a line typed at once is echoed in one send, then the line break "
         "and the prompt");
  before = sends;
  said = line("string.rep('a', 150)");
  expect(strstr(said, "=> \"aaaaaaaaaa") != NULL
         && strstr(said, "aaaaaaaaaa\"\r\n> ") != NULL
         && count(said, "a") == 150 + 1,
         "a result longer than 64 characters goes out whole, in pieces");
}

// A paste of 248 characters while a command runs, read in one pass
static void paste_a_comment(void) {
  static char text[256];
  memset(text, 0, sizeof text);
  strcpy(text, "--");
  memset(text + 2, 'b', 246);
  strcat(text, "\r");
  type_in(text);
}

// What a big result, or a paste, costs the heap: printing a string takes
// little more than the string, and a paste leaves nothing behind
static void big_output_and_paste(void) {
  const char *said;
  size_t before;
  char what[160];
  fresh();
  boot("");
  line("x = string.rep('a', 4000)");
  lua_gc(board_L, LUA_GCCOLLECT, 0);
  heap_limit = heap_used + 12000;
  said = line("print(x)");
  heap_limit = (size_t)-1;
  expect(count(said, "a") >= 4000 && strstr(said, "not enough memory") == NULL,
         "print() of 4,000 characters fits in 12 KB of heap to spare");
  line("x = string.rep('a', 1900)");
  lua_gc(board_L, LUA_GCCOLLECT, 0);
  heap_limit = heap_used + 12000;
  said = line("x");
  heap_limit = (size_t)-1;
  expect(count(said, "a") >= 1900 && strstr(said, "not enough memory") == NULL,
         "... and a 1,900-character result");
  line("x = nil");
  line("microbit.sleep(50)");
  lua_gc(board_L, LUA_GCCOLLECT, 0);
  before = heap_used;
  plan(paste_a_comment);
  line("microbit.sleep(50)");
  lua_gc(board_L, LUA_GCCOLLECT, 0);
  snprintf(what, sizeof what, "a 248-character paste read in one pass leaves "
           "%ld bytes of heap behind, under 1,000", (long)(heap_used - before));
  expect(heap_used < before + 1000, what);
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
  // A handler's mistake is shown without holding up the program, and goes
  // to the port, for when the display is busy
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
  expect(strstr(out, "\r\nRuntime error: program: oops\r\n")
         != NULL, "... and goes to the port");
  fresh();
  boot_program("error('at boot')\n");
  expect(strstr(out, "\r\nRuntime error: program: at boot"
                "\r\n<error Lua error!>") != NULL,
         "a mistake at boot goes to the port before it scrolls by");
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

// Events dropped for want of memory, for the line or for a fiber, are
// counted with those the full line drops, and the count outlives the line
static void press_a_with_no_memory(void) {
  board_refuses = 1;
  press_a();
  board_refuses = 0;
}

static void press_a_with_no_fiber(void) {
  miss(ID_BUTTON_A, CLICK);
}

static void every_drop_counted(void) {
  fresh();
  plan(press_a_with_no_memory);
  plan(press_a_with_no_fiber);
  plan(press_b);
  boot_program(
    "local got = 0\n"
    "function on_event(s, v) got = got + 1 end\n"
    "microbit.sleep(10) microbit.sleep(10) microbit.sleep(10)\n"
    "microbit.display.scroll(got .. ' ' .. microbit.eventsDropped())\n");
  expect(strstr(out, "<scroll 1 2>") != NULL,
         "a press with no memory for the line, and one with no fiber, are "
         "counted as dropped; one that waits is handled");
  fresh();
  plan(press_a_with_no_fiber);
  boot_program("microbit.sleep(10)\n"
               "microbit.display.scroll(microbit.eventsDropped())\n");
  expect(strstr(out, "<scroll 0>") != NULL,
         "... and a program with no on_event counts none");
}

// listen() serves a board that calls over the radio: a prompt, and the
// answer to a line. The session it serves is made then: booted, the
// firmware's script holds no more Lua heap than this, on the 64-bit host.
#define BOOTED_HEAP 45500

static void serving_a_link(void) {
  const char *said;
  char what[128];
  fresh();
  boot("");
  lua_gc(board_L, LUA_GCCOLLECT, 0);
  snprintf(what, sizeof what, "booted, the firmware's script holds %zu "
           "bytes of Lua heap, at most %d", heap_used, BOOTED_HEAP);
  expect(heap_used <= BOOTED_HEAP, what);
  caller_types = "6*7\r";
  said = line("listen('zezop')");
  expect(strcmp(caller_hears, "> => 42\n> ") == 0,
         "listen() gives a caller the prompt, and the answer to its line");
  expect(strstr(said, "hung up") != NULL && armed,
         "... and the prompt is back when the caller hangs up");
}

// The words a line the other micro:bit did not take is said to be lost in,
// and whether something typed after it went with it
static const char *not_taken(const char *text, int after, char *words,
                             size_t size) {
  snprintf(words, size, "\r\nThe other micro:bit did not answer, so it may "
           "not have got: %s\r\n%sIt may still be running a command. Once "
           "it has finished, check whether the line ran before you type it "
           "again.\r\n", text,
           after ? "What you typed after it was not sent either.\r\n" : "");
  return words;
}

// What the other board does while this one serves it: sends a move, then,
// while the robot drives, lines, and notes whether each was taken, how
// long that took, and what it had heard by then
static const char *far_types[4];
static int far_taken[4];
static uint32_t far_took_ms;
static char far_heard_then[1024];

static void far_sends_a_move(void) {
  far_says("robot_move(50, 50, 0.1)\n");
}

static void far_types_lines(void) {
  int i;
  uint32_t start = clock_ms;
  for (i = 0; far_types[i]; i++)
    far_taken[i] = far_says(far_types[i]);
  far_took_ms = clock_ms - start;
  snprintf(far_heard_then, sizeof far_heard_then, "%s", far_heard);
}

// The piece the other board sent last, sent again, as when this board's
// answer to it was lost
static void far_sends_again(void) {
  air_frame(&board_air, KIND_FIRST, LINK, far_link.out, "10+1\n", 5);
  board_fibers();
}

static void far_types_then_again(void) {
  int acks;
  far_types_lines();
  acks = board_acks;
  far_sends_again();
  far_taken[3] = board_acks == acks + 1;
}

// Serving a real link while the robot drives
static void lines_during_a_move(void) {
  char what[160];
  fresh();
  real_link = 1;
  boot("");
  far_calls();
  plan(far_sends_a_move);
  on_move = far_types_lines;
  far_types[0] = "10+1\n";
  far_types[1] = "20+2\n";
  far_types[2] = NULL;
  line("listen('gigat')");
  snprintf(what, sizeof what, "a line sent while the robot drives is taken "
           "at once, on the first try (%lu ms for two)",
           (unsigned long)far_took_ms);
  expect(far_taken[0] && far_took_ms < 30, what);
  expect(strcmp(far_heard_then, "> ") == 0,
         "... before the move has ended, and so before any prompt after it");
  expect(far_taken[1] && strcmp(far_heard, "> > => 11\n> => 22\n> ") == 0,
         "... and it runs after the move, the next line after it, each "
         "prompt after its line has run");

  // a piece sent again is answered again and dropped
  fresh();
  real_link = 1;
  boot("");
  far_calls();
  plan(far_sends_a_move);
  on_move = far_types_then_again;
  far_types[0] = "10+1\n";
  far_types[1] = NULL;
  line("listen('gigat')");
  expect(far_taken[0] && far_taken[3] && count(far_heard, "=> 11") == 1,
         "a piece that comes again is answered again, and runs once");
}

// While the robot drives, six lines take six of its inbox's eight pieces,
// and a line of 60 bytes the last two, finding no room for its third; once
// the robot has taken all the inbox held, the next line
static const char CUT_LINE[] =
  "hit = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'\n";
static int cut_lost, cut_done, next_sent;

static void far_fills_the_inbox(void) {
  static const char *const six[] = {
    "1\n", "2\n", "3\n", "4\n", "5\n", "6\n", NULL };
  int i;
  for (i = 0; six[i]; i++)
    far_says(six[i]);
  cut_lost = !far_says(CUT_LINE);
  cut_done = 1;
  board_took = 0;
}

static void far_says_the_next_line(void) {
  // the six lines and the two pieces
  if (cut_done && !next_sent && board_took == 8) {
    next_sent = 1;
    far_says("20+2\n");
  }
}

// A line that crosses a full inbox is lost whole: of it, nothing runs, and
// the line after it runs as it was sent
static void a_line_cut_short(void) {
  fresh();
  real_link = 1;
  boot("");
  far_calls();
  looks_left = 40;
  cut_lost = cut_done = next_sent = 0;
  plan(far_sends_a_move);
  on_move = far_fills_the_inbox;
  every_pause = far_says_the_next_line;
  expect(strlen(CUT_LINE) == 60, "the line is 60 bytes, three pieces");
  line("listen('gigat')");
  expect(cut_lost && next_sent,
         "a line that finds the inbox full after two of its pieces is lost");
  expect(strcmp(far_heard, "> > => 1\n> => 2\n> => 3\n> => 4\n> => 5\n"
                "> => 6\n> => 22\n> ") == 0,
         "... nothing of it runs, and the next line runs clean");
}

static void far_inbox(char *text, size_t size);

// A statement typed over several lines, over a real link: each line is a
// message of its own, and the lines of the statement still open stay
static void far_types_a_block(void) {
  far_says("for i = 1, 2 do\n");
  far_says("x = (x or 0) + i * 100\n");
  far_says("end\n");
  far_says("x\n");
}

static void a_block_over_the_link(void) {
  fresh();
  real_link = 1;
  boot("");
  far_calls();
  plan(far_types_a_block);
  line("listen('gigat')");
  expect(strcmp(far_heard, "> >> >> > => 300\n> ") == 0,
         "a statement over several lines runs whole over the link");
}

// A robot that serves a link, driving while the link fills the air: a
// button pressed meanwhile is handled when the move is over
static void press_a_then_lines(void) {
  int i;
  post(ID_BUTTON_A, CLICK);
  for (i = 0; i < 10; i++)
    far_says("1\n");
}

static void a_press_while_serving(void) {
  fresh();
  real_link = 1;
  boot("");
  far_calls();
  plan(far_sends_a_move);
  on_move = press_a_then_lines;
  line("listen('gigat')");
  expect(strstr(out, "<scroll A>") != NULL,
         "a press while the robot drives is kept, however much the link "
         "sends meanwhile");
}

// Twelve lines pasted at once on the board that called, each answered by
// the other board: every answer is printed, and none fails
static void a_paste_over_the_link(void) {
  char what[64];
  int i, all = 1;
  fresh();
  real_link = 1;
  far_listens = 1;
  boot("");
  line("connect('gigat', 100)");
  far_echoes = 1;
  type_in("1\r2\r3\r4\r5\r6\r7\r8\r9\r10\r11\r12\r");
  for (i = 1; i <= 12; i++) {
    snprintf(what, sizeof what, "=> %d\r\n", i);
    all = all && strstr(out, what) != NULL;
  }
  expect(all && far_echo_fails == 0 && strstr(out, "did not answer") == NULL,
         "twelve lines pasted over the link: every answer is printed");
}

// A line whose every answer is lost reached the other board all the same:
// it is said that it may not have got it, and what was typed after it
static void a_line_that_arrived(void) {
  char words[512], held[256];
  const char *said;
  fresh();
  real_link = 1;
  far_listens = 1;
  boot("");
  line("connect('gigat', 100)");
  far_busy = 1;
  far_unheard = 1;
  said = out + strlen(out);
  type_in("robot_move(50, 50, 1)\r1+1");
  expect(strstr(said, not_taken("robot_move(50, 50, 1)", 1, words,
                                sizeof words)) != NULL,
         "a line whose answers were all lost may not have been got, and "
         "what was typed after it was not sent either");
  far_busy = far_unheard = 0;
  far_inbox(held, sizeof held);
  expect(strcmp(held, "robot_move(50, 50, 1)\r") == 0,
         "... though the other board got it, as the words allow");
}

// Another pair on the group, whose link's number shares this one's low
// byte: its line does not run here, and its answers do not stand for the
// other board's
static void alike_sends_a_line(void) {
  air_frame(&board_air, KIND_FIRST, LINK_ALIKE, 1, "6*111\n", 6);
  board_fibers();
}

static void another_pair(void) {
  const char *said;
  fresh();
  real_link = 1;
  boot("");
  far_calls();
  plan(alike_sends_a_line);
  line("listen('gigat')");
  expect(strcmp(far_heard, "> ") == 0,
         "a line from another pair whose link's number shares this one's "
         "low byte does not run");
  fresh();
  real_link = 1;
  far_listens = 1;
  boot("");
  line("connect('gigat', 100)");
  far_gone = 1;
  alike_answers = 1;
  said = line("1+1");
  expect(strstr(said, "did not answer") != NULL,
         "... and its answers do not stand for the other board's");
}

// The caller calls again before the answer reaches it, and the answer to
// the second call comes after a line the link took: the line stays, and
// the session goes on
static void far_calls_again_after_a_line(void) {
  char hello[RADIO_NAME * 2 + 1] = "zezopgigat";
  hello[RADIO_NAME * 2] = VERSION;
  far_says("10+1\n");
  air_frame(&board_air, KIND_HELLO, LINK, 0, hello, sizeof hello);
  board_fibers();
}

static void a_call_repeated(void) {
  fresh();
  real_link = 1;
  boot("");
  far_calls();
  plan(far_calls_again_after_a_line);
  line("listen('gigat')");
  expect(strcmp(far_heard, "> => 11\n> ") == 0,
         "a call repeated for the open link keeps the line it took, and "
         "the session");
}

// Lines that find the other board's inbox full, one after another, for
// longer than the piece numbers of the link before went round: the next
// one is still not taken for one the other board has
static void numbers_that_come_round(void) {
  char held[64];
  int i, taken = 0, lost = 0;
  fresh();
  real_link = 1;
  boot("");
  radio_link_open(&board_link, LINK, "gigat");
  radio_link_open(&far_link, LINK, "zezop");
  far_busy = 1;
  for (i = 0; i < 8; i++)
    taken += radio_link_tx(&board_link, "1\n", 2);
  for (i = 0; i < 256; i++)
    lost += !radio_link_tx(&board_link, "2\n", 2);
  expect(taken == 8 && lost == 256 && !radio_link_tx(&board_link, "3\n", 2),
         "after 256 lines lost to a full inbox, the next is not taken for "
         "one the other board has");
  far_busy = 0;
  far_inbox(held, sizeof held);
  expect(strcmp(held, "1\n1\n1\n1\n1\n1\n1\n1\n") == 0,
         "... which holds the eight it took, and no other");
}

// Typing that reaches the port while a line fails to go goes with it
static void types_while_sending(void) {
  type_in("2+2\r");
}

static void typing_during_a_loss(void) {
  const char *said;
  fresh();
  real_link = 1;
  far_listens = 1;
  boot("");
  line("connect('gigat', 100)");
  far_gone = 1;
  on_air = types_while_sending;
  said = line("1+1");
  expect(count(said, "did not answer") == 1
         && strstr(said, "What you typed after it was not sent either.")
         && strstr(said, "may not have got: 2+2") == NULL,
         "what reaches the port while a line fails to go is dropped with "
         "it, and said to be");
}

// A line whose end reaches the port the moment the port is found empty
// goes all the same
static void a_line_at_the_last_moment(void) {
  fresh();
  real_link = 1;
  far_listens = 1;
  boot("");
  line("connect('gigat', 100)");
  empty_brings = "1+1\r";
  type_in("2+2\r");
  expect(strstr(far_heard, "2+2\r") && strstr(far_heard, "1+1\r"),
         "a line that reaches the port as it is found empty goes");
}

// A call from a board whose link is an older version is not answered:
// version 1's, with no version, and version 2's, whose head is 3 bytes
static void an_older_link(void) {
  static const uint8_t one[] = {
    KIND_HELLO, 7, 0, 'z', 'e', 'z', 'o', 'p', 'g', 'i', 'g', 'a', 't' };
  static const uint8_t two[] = {
    KIND_HELLO, 7, 0, 'z', 'e', 'z', 'o', 'p', 'g', 'i', 'g', 'a', 't', 2 };
  fresh();
  air_put(&board_air, one, sizeof one);
  air_put(&board_air, two, sizeof two);
  expect(!radio_link_called(&board_link, "zezop", NULL)
         && !radio_link_called(&board_link, "zezop", NULL)
         && board_link.link == 0 && far_air.count == 0,
         "a call from a board with an older link is not answered");
}

// The other board's inbox: what it holds, in order, taken out of it
static void far_inbox(char *text, size_t size) {
  uint8_t body[RADIO_BODY];
  int len;
  bool starts;
  text[0] = 0;
  while (radio_link_rx(&far_link, body, &len, &starts)) {
    size_t n = strlen(text);
    snprintf(text + n, size - n, "%.*s", len, (char *)body);
  }
}

// Lines sent over a real link to a robot that drives, whose Lua takes
// nothing meanwhile: its inbox takes eight pieces, and a line that finds it
// full is said to be lost, in words, and never arrives
static void a_full_inbox(void) {
  char words[256], held[256];
  const char *said;
  int i, taken = 1;
  fresh();
  real_link = 1;
  far_listens = 1;
  boot("");
  said = line("connect('gigat', 100)");
  expect(strstr(said, "gigat connected.") != NULL && far_link.link == LINK,
         "connect() calls the other board over a real link");
  neighbours = 1;
  {
    uint32_t start = clock_ms;
    said = line("0");
    expect(strstr(said, "did not answer") == NULL && clock_ms - start < 30,
           "... and a line goes on the first try while another pair talks "
           "on the air");
  }
  neighbours = 0;
  far_inbox(held, sizeof held);
  far_busy = 1;
  for (i = 1; i <= 8; i++) {
    char text[8];
    snprintf(text, sizeof text, "%d", i);
    said = line(text);
    taken = taken && strstr(said, "did not answer") == NULL;
  }
  expect(taken, "eight lines go while the other board drives");
  said = line("x = 9");
  expect(strstr(said, not_taken("x = 9", 0, words, sizeof words)) != NULL,
         "a line that finds the other board's inbox full is said to be "
         "lost, in words");
  far_busy = 0;
  far_inbox(held, sizeof held);
  expect(strcmp(held, "1\r2\r3\r4\r5\r6\r7\r8\r") == 0,
         "... the eight before it wait for the move to end, in order, and "
         "it never arrives");
}

// A line to a board gone from the air is said to be lost, in words, and
// what was typed after it goes with it
static void a_line_not_taken(void) {
  char words[256];
  const char *said;
  fresh();
  real_link = 1;
  far_listens = 1;
  boot("");
  line("connect('gigat', 100)");
  far_gone = 1;
  said = line("robot_move(50, 50, 1)");
  expect(strstr(said, not_taken("robot_move(50, 50, 1)", 0, words,
                                 sizeof words))
         != NULL, "a line to a board gone from the air is said to be lost, "
         "and how to send it again");
  far_gone = 0;
  far_busy = 0;
  said = line("1+1");
  expect(strstr(said, "did not answer") == NULL && strstr(far_heard, "1+1\r"),
         "... and the next line goes when it is back");
}

// The radio's inbox: made when the first link opens, not before; with
// none, a piece waits on the air for rx
static void the_radio_inbox(void) {
  uint8_t body[RADIO_BODY];
  int len = 0, before;
  bool starts = false;
  fresh();
  before = board_allocs;
  expect(board_link.inbox == NULL && !radio_link_rx(&board_link, body, &len, &starts)
         && board_allocs == before,
         "before any link, the radio keeps nothing and made nothing");
  radio_link_open(&board_link, LINK, "gigat");
  expect(board_allocs == before + 1 && board_link.inbox != NULL,
         "the first link makes the inbox");
  radio_link_open(&board_link, LINK, "gigat");
  expect(board_allocs == before + 1, "a link opened again makes no other");
  no_link();
  board_refuses = 1;
  radio_link_open(&board_link, LINK, "gigat");
  air_frame(&board_air, KIND_FIRST, LINK, 1, "hi", 2);
  radio_link_heard(&board_link);
  expect(board_acks == 0 && radio_link_rx(&board_link, body, &len, &starts)
         && len == 2 && body[0] == 'h' && starts && board_acks == 1,
         "with no inbox, rx takes a piece off the air, and answers it");
}

// A C stack too full to go deeper is an error a pcall catches, and the
// prompt comes back; the host's limit stands in for the board's 5,888 bytes
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

// Coroutines resumed one inside another, each from the one before: the
// C stack fills as with pcalls, and it stops the same way
static void deep_coroutines(void) {
  static const char *const RESUME[] = {
    "function deeper(n) depth = n "
    "return coroutine.wrap(function() return deeper(n + 1) end)() end",
    "function deeper(n) depth = n local co = coroutine.create(deeper) "
    "local ok, err = coroutine.resume(co, n + 1) "
    "if not ok then error(err, 0) end end",
    NULL };
  const char *said;
  char what[256];
  int i;
  for (i = 0; RESUME[i]; i++) {
    fresh();
    boot("");
    line(RESUME[i]);
    host_cstack_limit = host_cstack_used() + 40000;
    said = line("deeper(1)");
    host_cstack_limit = (size_t)-1;
    snprintf(what, sizeof what, "%s nested too deep stops with \"C stack "
             "overflow\", before Lua's own limit of 200 C calls",
             i == 0 ? "coroutine.wrap" : "coroutine.resume");
    expect(strstr(said, "C stack overflow") != NULL
           && strstr(line("depth < 150"), "=> true") != NULL, what);
  }
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL, "... and the REPL answers");
}

// A multiple assignment with many targets: the parser goes a level deeper
// for each, and stops within a level of the limit
static void many_targets(void) {
  char text[1024] = "a";
  int i, status;
  size_t limit;
  fresh();
  boot("");
  for (i = 0; i < 150; i++) strcat(text, ",a");
  strcat(text, " = 1");
  limit = host_cstack_used() + 3000;
  host_cstack_limit = limit;
  host_cstack_first_over = 0;
  status = luaL_loadstring(board_L, text);
  host_cstack_limit = (size_t)-1;
  expect(status != 0 && strstr(lua_tostring(board_L, -1), "C stack overflow")
         && host_cstack_first_over - limit < 1024,
         "an assignment to 151 targets stops with \"C stack overflow\" within "
         "a level of the limit");
  lua_pop(board_L, 1);
}

// A pattern whose match goes a C call deeper for each character it takes,
// as a? does, stops before it fills the C stack: in each function that
// matches
static void deep_patterns(void) {
  static const char *const MATCH[] = {
    "string.match(s, p)", "string.find(s, p)", "string.gsub(s, p, '')",
    "string.gmatch(s, p)()", NULL };
  const char *said;
  char what[256];
  int i;
  fresh();
  boot("");
  line("s = string.rep('a', 20000) p = string.rep('a?', 20000)");
  for (i = 0; MATCH[i]; i++) {
    host_cstack_limit = host_cstack_used() + 40000;
    said = line(MATCH[i]);
    host_cstack_limit = (size_t)-1;
    snprintf(what, sizeof what, "%s, 20,000 levels deep, stops with "
             "\"pattern too complex\"", MATCH[i]);
    expect(strstr(said, "pattern too complex") != NULL, what);
  }
  said = line("string.match('aab', 'a?a?b')");
  expect(strstr(said, "=> \"aab\"\r\n> ") != NULL,
         "... and a pattern that fits still matches");
}

// string.dump of a function whose own functions nest deep: its walk
// through them stops before it fills the C stack
static int dump_writer(lua_State *L, const void *p, size_t size, void *ud) {
  (void)L; (void)p; (void)size; (void)ud;
  return 0;
}

static int dump_deep(lua_State *L) {
  lua_getglobal(L, "f");
  host_cstack_limit = host_cstack_used() + 2000;
  lua_dump(L, dump_writer, NULL);
  return 0;
}

// Only text is loaded: precompiled code, which no check has read, is not
static void text_only(void) {
  const char *said;
  char f[4096] = "f = ";
  int i, status;
  fresh();
  boot("");
  said = line("loadstring(string.dump(function() return 42 end))");
  expect(strstr(said, "=> nil\t\"precompiled code cannot be loaded, "
                "only text\"") != NULL,
         "loadstring refuses precompiled code");
  for (i = 0; i < 60; i++) strcat(f, "function() return ");
  strcat(f, "1");
  for (i = 0; i < 60; i++) strcat(f, " end");
  if (luaL_dostring(board_L, f)) {
    fprintf(stderr, "%s\n", lua_tostring(board_L, -1));
    exit(2);
  }
  status = lua_cpcall(board_L, dump_deep, NULL);
  host_cstack_limit = (size_t)-1;
  expect(status != 0 && strstr(lua_tostring(board_L, -1), "C stack overflow")
         != NULL, "string.dump of functions nested too deep for the C stack "
         "stops with \"C stack overflow\"");
  lua_pop(board_L, 1);
  said = line("#string.dump(function() return 42 end) > 0");
  expect(strstr(said, "=> true\r\n> ") != NULL,
         "... and string.dump of a function that fits still dumps it");
}

// A robot_move whose Classic frame fails, the heap running out with it,
// after the Edu's has started the motors: both stops go out before the
// message is made
static void a_move_that_fails(void) {
  static const char FRAMES[] =
    "FF F9 10 03 32 32 00|01 32 32 00|FF F9 10 03 00 00 00|01 00 00 03|";
  const char *said;
  int status;
  fresh();
  boot("");
  bus_fails_at = 2;
  bus_starves = 1;
  lua_pushcfunction(board_L, tpbot_robot_move);
  lua_pushinteger(board_L, 50);
  lua_pushinteger(board_L, 50);
  lua_pushinteger(board_L, 1);
  status = lua_pcall(board_L, 3, 0, 0);
  heap_limit = (size_t)-1;
  lua_pop(board_L, 1);
  expect(status != 0 && strcmp(bus_log, FRAMES) == 0,
         "a move whose second frame fails as the heap runs out still stops "
         "both robots");
  bus_fails_at = 0;
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL, "... and the REPL answers");
}

// A handler that stops on a number as the heap runs out
static int stops_on_a_number(lua_State *L) {
  heap_limit = heap_used;
  lua_pushinteger(L, 9876543);
  return lua_error(L);
}

// Lua's heap, refusing only the message of an error thrown with none
static void *refuses_the_message(void *ud, void *ptr, size_t old,
                                 size_t size) {
  if (size > old
      && size == sizeof(TString) + sizeof "error in error handling")
    return NULL;
  return alloc(ud, ptr, old, size);
}

// A press, handled with 2 KB of the C stack already in use
static void press_a_deep(void) {
  volatile char taken[2048];
  taken[0] = 0;
  press_a();
  (void)taken[0];
}

// What goes wrong in a handler is shown with nothing taken from Lua's heap,
// out of the call that ran it: a mistake that is a number, as the heap runs
// out; and a C stack so full the error is thrown with no message, whose
// message there is no memory to make
static void mistakes_out_of_memory(void) {
  const char *said;
  fresh();
  boot("");
  lua_register(board_L, "on_event", stops_on_a_number);
  press_a();
  heap_limit = (size_t)-1;
  expect(strstr(out, "<error, going on 9876543>") != NULL,
         "a handler's mistake that is a number is shown as the heap runs out");
  lua_pushnil(board_L);
  lua_setglobal(board_L, "on_event");
  lua_setallocf(board_L, refuses_the_message, NULL);
  host_cstack_limit = 0;
  press_a_deep();
  host_cstack_limit = (size_t)-1;
  lua_setallocf(board_L, alloc, NULL);
  expect(strstr(out, "<error, going on error in error handling>") != NULL,
         "a handler thrown out with no message, and no memory to make one, "
         "is shown");
  said = line("6*7");
  expect(strstr(said, "=> 42\r\n> ") != NULL && panicked == 0,
         "... and the REPL answers");
}

// The scheduler's tick, as the firmware takes it: a port's event that
// waits while Lua is free is handed on again
static void tick(void) {
  if (lua_events_take_stranded_port())
    post(ID_SERIAL, HEAD_MATCH);
}

// A line typed at the idle prompt, whose port event found no fiber: the
// next tick brings it to the REPL
static void a_port_event_stranded(void) {
  const char *said;
  size_t before;
  fresh();
  boot("");
  before = strlen(out);
  type_and_miss_the_port();
  tick();
  expect(strstr(out + before, "6*7\r\r\n=> 42\r\n> ") != NULL && armed,
         "a port event that found no fiber while Lua was free is handed on "
         "at the next tick");
  said = line("6*8");
  expect(strstr(said, "=> 48\r\n> ") != NULL, "... and the REPL answers");
  // while a call runs, the call takes it, and the tick leaves it
  fresh();
  boot("");
  line("microbit.handler[1] = function() microbit.sleep(20) end");
  plan(type_and_miss_the_port);
  plan(tick);
  press_a();
  expect(count(out, "6*7\r\r\n=> 42") == 1,
         "... and one that waits for a running call is handed on once");
}

// A mistake in the script at boot, stripped of its lines, is named
// "program", with no line, whether the VM finds it, error() or assert()
// raises it, or a function it calls finds a bad argument; one a C
// function raises for another C function is not; a line typed at the REPL
// keeps its line
static void mistakes_named(void) {
  static const char *const RAISED[][2] = {
    { "error('boom')", "\r\nRuntime error: program: boom\r\n" },
    { "assert(false, 'nope')", "\r\nRuntime error: program: nope\r\n" },
    { "string.rep()", "\r\nRuntime error: program: bad argument #1 to "
      "'rep' (string expected, got no value)\r\n" },
    { "local ok, e = pcall(string.rep) error(e, 0)",
      "\r\nRuntime error: bad argument #1 to '?' (string expected, got no "
      "value)\r\n" },
    { NULL, NULL } };
  const char *said;
  char what[256];
  int i;
  for (i = 0; RAISED[i][0]; i++) {
    fresh();
    boot_program(RAISED[i][0]);
    snprintf(what, sizeof what, "%s, a mistake at boot, %s", RAISED[i][0],
             strstr(RAISED[i][1], "program: ") ? "is named \"program\""
             : "raised for a C function, has no name");
    expect(strstr(out, RAISED[i][1]) != NULL, what);
  }
  fresh();
  boot_program("local m = require('microbit')\n"
               "m.nothing.x = 1\n");
  expect(strstr(out, "\r\nRuntime error: program: attempt to index field "
                "'nothing' (a nil value)\r\n") != NULL
         && strstr(out, ":0:") == NULL,
         "a mistake in the stripped script at boot is named \"program\", "
         "with no line");
  // One the parser finds names its line, the lines being stripped only
  // once the script has loaded, through the loader main.cpp boots with
  fresh();
  boot_may_fail = 1;
  boot_alone("x = 1\ny = = 2\n");
  boot_may_fail = 0;
  expect(strstr(out, "\r\nCompile error: program:2: unexpected symbol near "
                "'='\r\n<error Compile error: ><error program:2: unexpected "
                "symbol near '='><booted>") != NULL,
         "a script at boot that does not compile is said so, naming its "
         "line: program:2: ...");
  {
    // One too big for the heap is said so, not as a mistake in it
    static char big[8192];
    int i, loaded;
    for (i = 0; i < 400; i++)
      strcat(big, "x = 1 ");
    fresh();
    boot("");
    lua_gc(board_L, LUA_GCCOLLECT, 0);
    heap_limit = heap_used + 2000;
    out[0] = 0;
    loaded = lua_events_boot_program(board_L, big, strlen(big), NULL);
    heap_limit = (size_t)-1;
    expect(!loaded && strstr(out, "\r\nToo big: the program does not fit in "
                             "the micro:bit's memory; make it smaller\r\n"
                             "<error Too big>")
           != NULL && strstr(out, "Compile error") == NULL,
           "a script too big for the heap is said to be, not a compile "
           "error");
  }
  fresh();
  boot("");
  said = line("local t t.x = 1");
  expect(strstr(said, "[string \"REPL\"]:1: attempt to index local 't'")
         != NULL, "... and one at the REPL keeps its line");
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
  // on a stack of 6 KB the limit is 2 KB lower
  fresh();
  boot("");
  stack_region = 6144;
  stack_used_now = 2300;
  plan(press_a);
  said = line("microbit.sleep(50)");
  expect(in_order(said, shallow),
         "on a 6 KB stack, a sleep 2,300 bytes deep runs handlers");
  stack_used_now = 2400;
  plan(press_a);
  said = line("microbit.sleep(50)");
  expect(in_order(said, deep) && count(said, "<scroll A>") == 1,
         "... and one 2,400 bytes deep leaves them for after the call");
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
  echo_in_pieces();
  big_output_and_paste();
  robot_file();
  round_two();
  what_waiting_costs();
  every_drop_counted();
  serving_a_link();
  lines_during_a_move();
  a_line_cut_short();
  a_block_over_the_link();
  a_press_while_serving();
  a_paste_over_the_link();
  a_line_that_arrived();
  another_pair();
  a_call_repeated();
  numbers_that_come_round();
  typing_during_a_loss();
  a_line_at_the_last_moment();
  an_older_link();
  a_full_inbox();
  a_line_not_taken();
  the_radio_inbox();
  a_full_c_stack();
  deep_coroutines();
  many_targets();
  deep_patterns();
  text_only();
  a_move_that_fails();
  mistakes_out_of_memory();
  a_port_event_stranded();
  mistakes_named();
  deep_sleeps();
  codex_round_nine();
  programs_alone();
  printf("%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
