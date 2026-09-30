// -*- mode: c; indent-tabs-mode: nil; -*-
//
// Events into Lua, one call at a time.
//
// The script at boot and every handler run on the one lua_State. A handler
// started in another fiber while a call sleeps would run on top of it, and
// the state breaks when the two end out of order. So an event that comes
// while Lua runs waits, in the order the events came, and is handled in one
// of two places:
//
// - at a safe point: when the running call enters microbit.sleep, it runs
//   the handlers of the events that were waiting then itself, on its own
//   fiber, each to its end, and again every 10 ms while it sleeps. Nested
//   so, a handler always ends before the call it runs in goes on. The
//   running call is the script at boot, or a command typed at the REPL
//   (the firmware's own handler of the port's event); a handler's own sleep
//   handles nothing, so it never nests another, and nor does a sleep that
//   is already deep in the stack (SAFE_POINT_STACK). A sleep ends on time
//   however many events come, but lasts as long as the handlers it runs;
// - when the running call is over.
//
// The line is made the first time an event has to wait, and only for a
// program that has somewhere to send it: an on_event, or a fallback. Its
// size is 16 unless microbit.eventLine() says otherwise; 0 drops every
// event that would have to wait. When it is full the oldest event in it is
// dropped, and counted (microbit.eventsDropped()). The port's event that
// the REPL waits for never is: the port is armed for one at a time, so one
// lost would leave the REPL deaf. A flag of its own keeps it, and it goes
// first when the running call is over; a sleep leaves it waiting, so the
// lines sent to the REPL run one after another, each after the one before
// has ended. Events that are only noise to Lua, as the firmware lists
// them, are dropped as they come.
//
// Fibers take turns only where one sleeps or waits, and everything here
// runs in fibers, so nothing comes between a look at the line and what is
// done on it.

#include <stdbool.h>

#include "lua.h"
#include "lauxlib.h"
#include "board-alloc.h"
#include "lua-events.h"

// How long a sleep that handles events sleeps at a time, in ms
#define SLICE 10

// The deepest a sleep may be, in bytes of C stack in use, and still run
// handlers: they have 1.5 KB before Lua's C stack check (lua-cstack.c).
#define SAFE_POINT_STACK 4352

// The waiting line, made the first time an event has to wait for a
// program that has somewhere to send it, of the size microbit.eventLine()
// last gave (16 unless it said otherwise).
typedef struct {
  int size, first, count;
  uint32_t dropped;
  LuaEvent waiting[];
} Line;

static lua_State *state;
static const LuaEventsConfig *config;
static Line *line;
static uint8_t line_size = 16;
static bool running = false;
static bool sleep_handles = false;   // the running call's sleeps handle events
static bool port_call = false;       // the port's event is being handled
static bool wants = false;           // there is on_event, or a fallback
static volatile bool port_waiting = false;

// The registry's key for the handler used when on_event is not a function
static char fallback_key;

void lua_events_open(lua_State *L, const LuaEventsConfig *c) {
  state = L;
  config = c;
  running = sleep_handles = wants = port_waiting = false;
  board_free(line);
  line = NULL;
  line_size = 16;
}

bool lua_event_is_noise(uint16_t source, uint16_t value) {
  int i;
  for (i = 0; i < config->noises; i++)
    if (config->noise[i].source == source && config->noise[i].value == value)
      return true;
  return false;
}

// The port's event carries nothing but that it came, so one flag holds it,
// set from an interrupt as safely as from a fiber.
void lua_events_port_missed(void) {
  port_waiting = true;
}

static bool is_port(LuaEvent e) {
  return e.source == config->port.source && e.value == config->port.value;
}

static Line *make_line(int size) {
  Line *l = (Line *)board_alloc(sizeof(Line) + size * sizeof(LuaEvent));
  if (l != NULL) {
    l->size = size;
    l->first = l->count = 0;
    l->dropped = 0;
  }
  return l;
}

// An event onto the line; when it is full, the oldest is dropped
static void append(Line *l, LuaEvent e) {
  if (l->count == l->size) {
    l->dropped++;
    if (l->size == 0)
      return;
    l->first = (l->first + 1) % l->size;
    l->count--;
  }
  l->waiting[(l->first + l->count) % l->size] = e;
  l->count++;
}

static void queue_event(LuaEvent e) {
  if (is_port(e)) {
    port_waiting = true;
    return;
  }
  if (!wants)
    return;
  if (line == NULL && (line = make_line(line_size)) == NULL)
    return;
  append(line, e);
}

// The next event to handle. The port's goes first, but never at a
// sleep: a command's lines follow one another, so the next one waits for
// the one that sleeps to end.
static bool next_event(LuaEvent *e, bool port_too) {
  if (port_waiting && port_too) {
    e->source = config->port.source;
    e->value = config->port.value;
    e->timestamp = 0;
    port_waiting = false;
    return true;
  }
  if (line == NULL || line->count == 0)
    return false;
  *e = line->waiting[line->first];
  line->first = (line->first + 1) % line->size;
  line->count--;
  return true;
}

// Whether the value at `at` can be called: a function, or a value whose
// metatable's __call is one. Read raw: nothing a program wrote runs here.
static bool callable(lua_State *L, int at) {
  bool yes;
  if (lua_isfunction(L, at))
    return true;
  if (!luaL_getmetafield(L, at, "__call"))
    return false;
  yes = lua_isfunction(L, -1);
  lua_pop(L, 1);
  return yes;
}

typedef struct {
  LuaEvent e;
  bool port_call;   // the handling of the port's event, a REPL command
} Call;

// The call for one event, run protected: on_event, or when that cannot be
// called, the handler the script gave microbit.eventFallback(). A value
// put in on_event by mistake leaves the REPL answering, so the mistake can
// be put right from the prompt. With neither, the event goes nowhere.
// on_event is read raw from the globals: a metamethod of _G's has no say.
// Its sleeps handle nothing, unless the REPL says, through
// microbit.eventRepl(), that it runs a command for the port's event.
static int call_handler(lua_State *L) {
  Call *c = (Call *)lua_touserdata(L, 1);
  lua_pushlightuserdata(L, &fallback_key);
  lua_rawget(L, LUA_REGISTRYINDEX);
  lua_pushliteral(L, "on_event");
  lua_rawget(L, LUA_GLOBALSINDEX);
  if (!callable(L, 3)) {
    lua_pop(L, 1);
    lua_pushvalue(L, 2);
    if (!callable(L, 3))
      return 0;
  }
  sleep_handles = false;
  lua_pushinteger(L, c->e.source);
  lua_pushinteger(L, c->e.value);
  lua_pushinteger(L, (lua_Integer)c->e.timestamp);
  lua_call(L, 3, 0);
  return 0;
}

// A mistake in a handler is shown without waiting for it to scroll by:
// the call that is running goes on.
static void handle(LuaEvent e, bool port) {
  bool outer = sleep_handles, outer_port = port_call;
  Call c;
  c.e = e;
  c.port_call = port;
  port_call = port;
  if (lua_cpcall(state, call_handler, &c) != 0) {
    const char *err = lua_tostring(state, -1);
    if (err)
      lua_events_show_error(err, false);
    lua_pop(state, 1);
  }
  sleep_handles = outer;
  port_call = outer_port;
}

static int look_for_on_event(lua_State *L) {
  lua_pushliteral(L, "on_event");
  lua_rawget(L, LUA_GLOBALSINDEX);
  wants = callable(L, -1);
  return 0;
}

// Whether a program has somewhere for events to go, looked at only
// between calls and at safe points, where Lua is this fiber's: until it
// has, an event that would have to wait is dropped, and no line is made.
static void look_for_a_handler(void) {
  if (wants)
    return;
  lua_pushlightuserdata(state, &fallback_key);
  lua_rawget(state, LUA_REGISTRYINDEX);
  wants = lua_isfunction(state, -1);
  lua_pop(state, 1);
  if (!wants && lua_cpcall(state, look_for_on_event, NULL) != 0)
    lua_pop(state, 1);
}

// Before any wait that lets other fibers run while Lua runs: a program
// that has just set on_event has somewhere for the events of that wait to
// go, and they are kept for it. Looked at here, in the fiber that owns
// Lua, and at once when there is a handler already.
void lua_events_before_wait(void) {
  if (running)
    look_for_a_handler();
}

void lua_call_begin(void) {
  running = true;
  sleep_handles = true;
}

// What waits, each handled to its end, the port's first; then Lua is free.
void lua_call_end(void) {
  LuaEvent e;
  sleep_handles = false;
  look_for_a_handler();
  while (next_event(&e, true))
    handle(e, is_port(e));
  running = false;
}

void lua_event_arrived(LuaEvent e) {
  if (lua_event_is_noise(e.source, e.value))
    return;
  if (running) {
    queue_event(e);
    return;
  }
  running = true;
  handle(e, is_port(e));
  lua_call_end();
}

// At a safe point, the events that were waiting when it was reached:
// those a handler raises meanwhile wait for the next one, so a sleep ends
// on time however fast they come.
static void handle_what_waits(void) {
  int n = line != NULL ? line->count : 0;
  LuaEvent e;
  while (n-- > 0 && next_event(&e, false))
    handle(e, false);
}

void lua_events_sleep(uint32_t ms) {
  uint32_t start, elapsed;
  bool paused = false;
  lua_events_before_wait();
  if (!running || !sleep_handles
      || lua_events_stack_used() > SAFE_POINT_STACK) {
    lua_events_pause(ms);
    return;
  }
  start = lua_events_now();
  for (;;) {
    handle_what_waits();
    elapsed = lua_events_now() - start;
    if (elapsed >= ms)
      break;
    lua_events_pause(ms - elapsed < SLICE ? ms - elapsed : SLICE);
    paused = true;
  }
  // sleep(0), or a sleep its handlers outlasted, still lets the other
  // fibers run
  if (!paused)
    lua_events_pause(0);
}

void lua_events_boot(lua_State *L) {
  lua_call_begin();
  if (lua_pcall(L, 0, 0, 0) != 0) {
    const char *err = lua_tostring(L, -1);
    lua_events_show_error("Lua error!", true);
    if (err)
      lua_events_show_error(err, true);
    lua_pop(L, 1);
  }
  lua_call_end();
}

// microbit.eventRepl(): the REPL runs a command for the port's event, and
// its sleeps are safe points, as the script's at boot are. It holds only
// while the port's event is being handled as the running call, so it lets
// no handler run inside another; whatever on_event passed the event on to
// the REPL, the REPL's own or one a program put in front of it, the
// command's sleeps handle events.
int lua_events_repl(lua_State *L) {
  (void)L;
  if (port_call)
    sleep_handles = true;
  return 0;
}

int lua_events_dropped(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)(line != NULL ? line->dropped : 0));
  return 1;
}

// How many events may wait, from 0, where every event that would have to
// wait is dropped, to 255. A line already made is made anew, keeping its
// newest events.
int lua_events_line(lua_State *L) {
  int n = luaL_checkint(L, 1);
  luaL_argcheck(L, 0 <= n && n <= 255, 1, "from 0 to 255");
  line_size = (uint8_t)n;
  if (line != NULL && line->size != n) {
    Line *old = line, *made = make_line(n);
    LuaEvent e;
    if (made == NULL)
      return luaL_error(L, "not enough memory");
    made->dropped = old->dropped;
    while (old->count > 0) {
      e = old->waiting[old->first];
      old->first = (old->first + 1) % old->size;
      old->count--;
      append(made, e);
    }
    line = made;
    board_free(old);
  }
  return 0;
}

// The first handler given stays: the firmware's script gives its own.
int lua_events_fallback(lua_State *L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_pushlightuserdata(L, &fallback_key);
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (lua_isnil(L, -1)) {
    lua_pushlightuserdata(L, &fallback_key);
    lua_pushvalue(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
  }
  wants = true;
  return 0;
}
