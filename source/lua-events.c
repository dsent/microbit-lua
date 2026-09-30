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
//   the handlers that wait itself, on its own fiber, each to its end, and
//   again whenever one comes while it sleeps. Nested so, a handler always
//   ends before the call it runs in goes on. The running call is the
//   script at boot, or a command typed at the REPL (the handling of the
//   port's event); a handler's own sleep runs nothing, so it never nests
//   another;
// - when the running call is over.
//
// The line is short and fixed: when it is full the oldest event in it is
// dropped, and counted (microbit.eventsDropped()). The port's event that
// the REPL waits for never is: the port is armed for one at a time, so one
// lost would leave the REPL deaf. It waits apart, one at most, and goes
// first when the running call is over; a sleep leaves it waiting, so the
// lines sent to the REPL run one after another, each after the one before
// has ended.
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
static uint16_t port_source, port_value;
static bool running = false;
static bool sleep_handles = false;   // the running call's sleeps handle events
static LuaEvent waiting[WAITING];
static int first = 0, count = 0;
static bool port_waiting = false;
static LuaEvent port_event;
static uint32_t dropped = 0;

// The registry's key for the handler used when on_event is not a function
static char fallback_key;

void lua_events_open(lua_State *L, uint16_t source, uint16_t value) {
  state = L;
  port_source = source;
  port_value = value;
  running = sleep_handles = port_waiting = false;
  first = count = 0;
  dropped = 0;
}

static bool is_port(LuaEvent e) {
  return e.source == port_source && e.value == port_value;
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

// Whether the value on top can be called: a function, or a value whose
// metatable has __call. Read raw: nothing a program wrote runs here.
static bool callable(void) {
  if (lua_isfunction(state, -1))
    return true;
  if (luaL_getmetafield(state, -1, "__call")) {
    lua_pop(state, 1);
    return true;
  }
  return false;
}

// on_event, or when that cannot be called, the handler the script gave
// microbit.eventFallback(): a value put in on_event by mistake leaves the
// REPL answering, so the mistake can be put right from the prompt. With
// neither, the event goes nowhere. on_event is read raw from the globals,
// since a metamethod of _G's that raised an error here, outside any
// protected call, would stop the board.
static void handle(LuaEvent e) {
  lua_pushliteral(state, "on_event");
  lua_rawget(state, LUA_GLOBALSINDEX);
  if (!callable()) {
    lua_pop(state, 1);
    lua_pushlightuserdata(state, &fallback_key);
    lua_rawget(state, LUA_REGISTRYINDEX);
    if (!lua_isfunction(state, -1)) {
      lua_pop(state, 1);
      return;
    }
  }
  lua_pushinteger(state, e.source);
  lua_pushinteger(state, e.value);
  lua_pushinteger(state, (lua_Integer)e.timestamp);
  if (lua_pcall(state, 3, 0, 0) != 0) {
    const char *err = lua_tostring(state, -1);
    if (err)
      lua_events_show_error(err);
    lua_pop(state, 1);
  }
}

// The events that wait, each handled to its end; their sleeps handle none.
static void handle_waiting(bool port_too) {
  bool outer = sleep_handles;
  LuaEvent e;
  sleep_handles = false;
  while (next_event(&e, port_too))
    handle(e);
  sleep_handles = outer;
}

void lua_call_begin(void) {
  running = true;
  sleep_handles = true;
}

void lua_call_end(void) {
  sleep_handles = false;
  handle_waiting(true);
  running = false;
}

void lua_event_arrived(LuaEvent e) {
  if (running) {
    queue_event(e);
    return;
  }
  running = true;
  sleep_handles = is_port(e);
  handle(e);
  lua_call_end();
}

void lua_events_sleep(uint32_t ms) {
  uint32_t start, elapsed;
  if (!running || !sleep_handles) {
    lua_events_pause(ms);
    return;
  }
  start = lua_events_now();
  for (;;) {
    handle_waiting(false);
    elapsed = lua_events_now() - start;
    if (elapsed >= ms)
      return;
    lua_events_pause(ms - elapsed < SLICE ? ms - elapsed : SLICE);
  }
}

void lua_events_boot(lua_State *L) {
  lua_call_begin();
  if (lua_pcall(L, 0, 0, 0) != 0) {
    const char *err = lua_tostring(L, -1);
    lua_events_show_error("Lua error!");
    if (err)
      lua_events_show_error(err);
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
