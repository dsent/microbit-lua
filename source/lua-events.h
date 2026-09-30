// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef LUA_EVENTS_H
#define LUA_EVENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "lua.h"

typedef struct {
  uint16_t source;
  uint16_t value;
  uint64_t timestamp;
} LuaEvent;

// L, and which event is the port's that the REPL waits for.
void lua_events_open(lua_State *L, uint16_t port_source, uint16_t port_value);

// An event, from a fiber of its own: handled now, or when Lua is free.
void lua_event_arrived(LuaEvent e);

// Around a call into Lua from outside an event, such as the script at
// boot: events that come meanwhile wait, and lua_call_end() handles them.
void lua_call_begin(void);
void lua_call_end(void);

// microbit.eventsDropped() and microbit.eventFallback(f)
int lua_events_dropped(lua_State *L);
int lua_events_fallback(lua_State *L);

// Supplied by the firmware, or by the host test in its place: a handler's
// mistake, shown.
void lua_events_show_error(const char *message);

#ifdef __cplusplus
}
#endif

#endif
