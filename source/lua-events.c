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
//   handles nothing, so it never nests another. A sleep ends on time
//   however many events come, but lasts as long as the handlers it runs;
// - when the running call is over.
//
// The line is short and fixed: when it is full the oldest event in it is
// dropped, and counted (microbit.eventsDropped()). The port's event that
// the REPL waits for never is: the port is armed for one at a time, so one
// lost would leave the REPL deaf. It waits apart, one at most, and goes
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
#include "lua-events.h"

#define WAITING 16

// How long a sleep that handles events sleeps at a time, in ms
#define SLICE 10

static lua_State *state;
static LuaEventId port;
static const LuaEventId *noise;
static int noises;
static bool running = false;
static bool sleep_handles = false;   // the running call's sleeps handle events
static LuaEvent waiting[WAITING];
static int first = 0, count = 0;
static bool port_waiting = false;
static volatile bool port_missed = false;
static LuaEvent port_event;
static uint32_t dropped = 0;

// The registry's key for the handler used when on_event is not a function
static char fallback_key;

void lua_events_open(lua_State *L, LuaEventId port_id,
                     const LuaEventId *noise_ids, int n) {
  state = L;
  port = port_id;
  noise = noise_ids;
  noises = n;
  running = sleep_handles = port_waiting = port_missed = false;
  first = count = 0;
  dropped = 0;
}

bool lua_event_is_noise(uint16_t source, uint16_t value) {
  int i;
  for (i = 0; i < noises; i++)
    if (noise[i].source == source && noise[i].value == value)
      return true;
  return false;
}

void lua_events_port_missed(void) {
  port_missed = true;
}

static bool is_port(LuaEvent e) {
  return e.source == port.source && e.value == port.value;
}

static void queue_event(LuaEvent e) {
  if (is_port(e)) {
    port_event = e;
    port_waiting = true;
    return;
  }
  if (count == WAITING) {
    first = (first + 1) % WAITING;
    count--;
    dropped++;
  }
  waiting[(first + count) % WAITING] = e;
  count++;
}

// The next event to handle. The port's goes first, but never at a
// sleep: a command's lines follow one another, so the next one waits for
// the one that sleeps to end.
static bool next_event(LuaEvent *e, bool port_too) {
  if (port_missed) {
    LuaEvent missed = { port.source, port.value, 0 };
    port_missed = false;
    port_event = missed;
    port_waiting = true;
  }
  if (port_waiting && port_too) {
    *e = port_event;
    port_waiting = false;
    return true;
  }
  if (count == 0)
    return false;
  *e = waiting[first];
  first = (first + 1) % WAITING;
  count--;
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
// The sleeps of a REPL command handle events; that is the port's event
// going to the firmware's own handler. A program's own on_event gets it
// as any other event, and its sleeps handle none.
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
  sleep_handles = c->port_call && lua_rawequal(L, 2, 3);
  lua_pushinteger(L, c->e.source);
  lua_pushinteger(L, c->e.value);
  lua_pushinteger(L, (lua_Integer)c->e.timestamp);
  lua_call(L, 3, 0);
  return 0;
}

// A mistake in a handler is shown without waiting for it to scroll by:
// the call that is running goes on.
static void handle(LuaEvent e, bool port_call) {
  bool outer = sleep_handles;
  Call c;
  c.e = e;
  c.port_call = port_call;
  if (lua_cpcall(state, call_handler, &c) != 0) {
    const char *err = lua_tostring(state, -1);
    if (err)
      lua_events_show_error(err, false);
    lua_pop(state, 1);
  }
  sleep_handles = outer;
}

void lua_call_begin(void) {
  running = true;
  sleep_handles = true;
}

// What waits, each handled to its end, the port's first; then Lua is free.
void lua_call_end(void) {
  LuaEvent e;
  sleep_handles = false;
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
  int n = count;
  LuaEvent e;
  while (n-- > 0 && next_event(&e, false))
    handle(e, false);
}

void lua_events_sleep(uint32_t ms) {
  uint32_t start, elapsed;
  bool paused = false;
  if (!running || !sleep_handles) {
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

int lua_events_dropped(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)dropped);
  return 1;
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
  return 0;
}
