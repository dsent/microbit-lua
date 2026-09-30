// -*- mode: c; indent-tabs-mode: nil; -*-
//
// Events into Lua, one call at a time.
//
// The script at boot and every handler run on the one lua_State. A handler
// started while another call sleeps would run on top of it, and the state
// breaks when the two end out of order. So an event that comes while Lua
// runs waits, and is handled when the call is over, in the order the
// events came. The line is short and fixed: an event that finds it full is
// dropped, and counted (microbit.eventsDropped()). The port's event that
// the REPL waits for never is: the port is armed for one at a time, so one
// lost would leave the REPL deaf. It waits apart, one at most, and goes
// first.
//
// Fibers take turns only where one sleeps or waits, and everything here
// runs in fibers, so nothing comes between a look at the line and what is
// done on it.

#include <stdbool.h>

#include "lua.h"
#include "lauxlib.h"
#include "lua-events.h"

#define WAITING 16

static lua_State *state;
static uint16_t port_source, port_value;
static bool running = false;
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
}

static void queue_event(LuaEvent e) {
  if (e.source == port_source && e.value == port_value) {
    port_event = e;
    port_waiting = true;
  } else if (count == WAITING) {
    dropped++;
  } else {
    waiting[(first + count) % WAITING] = e;
    count++;
  }
}

static bool next_event(LuaEvent *e) {
  if (port_waiting) {
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

// on_event, or when that is not a function, the handler the script gave
// microbit.eventFallback(): a value put in on_event by mistake leaves the
// REPL answering, so the mistake can be put right from the prompt.
static void handle(LuaEvent e) {
  lua_getglobal(state, "on_event");
  if (!lua_isfunction(state, -1)) {
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

void lua_call_begin(void) {
  running = true;
}

void lua_call_end(void) {
  LuaEvent e;
  while (next_event(&e))
    handle(e);
  running = false;
}

void lua_event_arrived(LuaEvent e) {
  if (running) {
    queue_event(e);
    return;
  }
  lua_call_begin();
  handle(e);
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
