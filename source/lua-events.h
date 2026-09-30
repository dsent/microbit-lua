// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef LUA_EVENTS_H
#define LUA_EVENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "lua.h"

// The timestamp is what Lua sees of CODAL's: its low 32 bits.
typedef struct {
  uint16_t source;
  uint16_t value;
  uint32_t timestamp;
} LuaEvent;

typedef struct {
  uint16_t source;
  uint16_t value;
} LuaEventId;

// Which event is the port's that the REPL waits for, and the events that
// are noise to Lua, dropped as they come
typedef struct {
  LuaEventId port;
  const LuaEventId *noise;
  int noises;
} LuaEventsConfig;

// L, and the config, which lives as long as the state
void lua_events_open(lua_State *L, const LuaEventsConfig *config);

// Whether an event is one of the noise, not worth a fiber
bool lua_event_is_noise(uint16_t source, uint16_t value);

// An event came, but could not be handed on: the port's is taken as
// waiting, any other counted as dropped. Safe to call from an interrupt.
void lua_events_missed(LuaEvent e);

// Whether the port's event waits while Lua is free, with no call to take
// it; true takes it, to be handed on again. Safe to call from an interrupt.
bool lua_events_take_stranded_port(void);

// An event, from a fiber of its own: handled now, or when Lua is free.
void lua_event_arrived(LuaEvent e);

// Around a call into Lua from outside an event: events that come
// meanwhile wait, its sleeps handle them, and lua_call_end() handles the
// rest.
void lua_call_begin(void);
void lua_call_end(void);

// The script at boot, loaded and on top of the stack, run as such a call;
// a mistake goes to the port and is shown ("Lua error!", then the
// message), and the board goes on handling events.
void lua_events_boot(lua_State *L);

// microbit.sleep(ms): a safe point, where the running call handles the
// events that wait (see source/lua-events.c).
void lua_events_sleep(uint32_t ms);

// Every other binding that waits, letting other fibers run, calls this
// first, in the fiber that runs Lua: an on_event the program has just set
// then gets the events that come while it waits. tests/wait-audit.sh
// checks the bindings in source/codal-lua.cpp.
void lua_events_before_wait(void);

// microbit.eventsDropped(), microbit.eventFallback(f) and
// microbit.eventLine(n)
int lua_events_dropped(lua_State *L);
int lua_events_repl(lua_State *L);
int lua_events_fallback(lua_State *L);
int lua_events_line(lua_State *L);

// Supplied by the firmware, or by the host test in its place: a mistake,
// shown, waiting for it to be seen or not; the time in ms; and a sleep that
// lets other fibers run.
void lua_events_show_error(const char *message, bool wait);

// A mistake, written to the serial port on a line of its own, "Runtime
// error: " first, with nothing taken from the heap
void lua_events_port_error(const char *message);

// The port armed for its next event, as microbit.serial.eventAfterAsync(1)
// arms it
void lua_events_arm_port(void);

// The C stack in use, in bytes
uint32_t lua_events_stack_used(void);
uint32_t lua_events_now(void);
void lua_events_pause(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif
