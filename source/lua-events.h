// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef LUA_EVENTS_H
#define LUA_EVENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "lua.h"

// The timestamp is what Lua sees of CODAL's: its low 32 bits.
typedef struct {
  uint16_t source;
  uint16_t value;
  uint32_t timestamp;
} LuaEvent;

// L, and which event is the port's that the REPL waits for.
void lua_events_open(lua_State *L, uint16_t port_source, uint16_t port_value);

// An event, from a fiber of its own: handled now, or when Lua is free.
void lua_event_arrived(LuaEvent e);

// Around a call into Lua from outside an event: events that come
// meanwhile wait, its sleeps handle them, and lua_call_end() handles the
// rest.
void lua_call_begin(void);
void lua_call_end(void);

// The script at boot, loaded and on top of the stack, run as such a call;
// a mistake is shown ("Lua error!", then the message) and the board goes
// on handling events.
void lua_events_boot(lua_State *L);

// microbit.sleep(ms): a safe point, where the running call handles the
// events that wait (see source/lua-events.c).
void lua_events_sleep(uint32_t ms);

// microbit.eventsDropped() and microbit.eventFallback(f)
int lua_events_dropped(lua_State *L);
int lua_events_fallback(lua_State *L);

// Supplied by the firmware, or by the host test in its place: a mistake,
// shown; the time in ms; and a sleep that lets other fibers run.
void lua_events_show_error(const char *message);
uint32_t lua_events_now(void);
void lua_events_pause(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif
