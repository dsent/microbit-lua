// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef LUA_EVENTS_H
#define LUA_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "lua.h"

typedef struct {
  uint16_t source;
  uint16_t value;
  uint32_t timestamp;
} LuaEvent;

// L, and the serial port's event that brings the REPL what was typed
void lua_events_open(lua_State *L, uint16_t port_source, uint16_t port_value);

// An event, in a fiber of its own: handled now, or when Lua is free
void lua_event_arrived(LuaEvent e);

// Whether an event of source and value waits for Lua
bool lua_event_waits(uint16_t source, uint16_t value);

// Whether an event other than the serial port's that arrives now is taken,
// to be handled now or to wait, rather than dropped
bool lua_event_room(void);

// Around a call into Lua that is no event's, the script at boot: events
// that come meanwhile wait for its sleeps, or for its end
void lua_call_begin(void);
void lua_call_end(void);

// microbit.sleep(ms): a safe point, where the running call handles the
// events that wait
void lua_events_sleep(uint32_t ms);

// Supplied by the firmware, or by the host test in its place: a handler's
// mistake, said on the port and shown on the display; the time in ms; and
// a sleep that lets other fibers run.
void lua_events_mistake(const char *message);
uint32_t lua_events_now(void);
void lua_events_pause(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif
