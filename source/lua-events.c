// -*- mode: c; indent-tabs-mode: nil; -*-
//
// Events into Lua, one call at a time.
//
// The script at boot and every handler run on the one lua_State. A handler
// started in a fiber of its own while another call waits, in a sleep, a
// scroll or a robot move, would run on top of that call, and the state
// breaks when the two end out of order. So an event that comes while Lua
// runs waits, in the order the events came, and is handled in one of two
// places:
//
// - at a safe point: when the running call enters microbit.sleep, it runs
//   the handlers of the events waiting then itself, each to its end, and
//   again every 10 ms while it sleeps. The running call is the script at
//   boot, or the serial port's event, which brings the REPL its line. A
//   handler's own sleep handles nothing, so handlers never nest deeper
//   than one;
// - when the running call is over.
//
// The line holds 16 events; one that comes when it is full is dropped.
// The port's event is kept apart, in a flag: the port is armed for one
// event at a time, so the REPL would never hear of what is typed if that
// one were dropped. It is handled first when the running call is over, and
// never at a sleep, so the lines sent to the REPL run one after another.
//
// Fibers take turns only where one sleeps or waits, and everything here
// runs in fibers, so nothing comes between a look at the line and what is
// done on it.

#include "lua.h"
#include "lua-events.h"

// How long a sleep that handles events sleeps at a time, in ms
#define SLICE 10
// How many events wait at most
#define LINE 16

static lua_State *state;
static LuaEvent port;
static LuaEvent line[LINE];
static int first, waiting;
static bool running;        // a call is in Lua
static bool sleep_handles;  // the running call's sleeps handle events
static bool port_waiting;

void lua_events_open(lua_State *L, uint16_t port_source, uint16_t port_value) {
  state = L;
  port.source = port_source;
  port.value = port_value;
  port.timestamp = 0;
  first = waiting = 0;
  running = sleep_handles = port_waiting = false;
}

static bool is_port(LuaEvent e) {
  return e.source == port.source && e.value == port.value;
}

static LuaEvent take(void) {
  LuaEvent e = line[first];
  first = (first + 1) % LINE;
  waiting--;
  return e;
}

static int call_on_event(lua_State *L) {
  LuaEvent *e = (LuaEvent *)lua_touserdata(L, 1);
  lua_getglobal(L, "on_event");
  if (!lua_isfunction(L, -1))
    return 0;
  lua_pushinteger(L, e->source);
  lua_pushinteger(L, e->value);
  lua_pushinteger(L, (lua_Integer)e->timestamp);
  lua_call(L, 3, 0);
  return 0;
}

// on_event for one event, run protected. Its sleeps handle the events
// that wait only when it is the port's, a line at the REPL. A mistake is
// said, and the call that is running, if any, goes on.
static void handle(LuaEvent e) {
  bool outer = sleep_handles;
  sleep_handles = is_port(e);
  if (lua_cpcall(state, call_on_event, &e) != 0) {
    const char *err = lua_tostring(state, -1);
    if (err)
      lua_events_mistake(err);
    lua_pop(state, 1);
  }
  sleep_handles = outer;
}

void lua_call_begin(void) {
  running = true;
  sleep_handles = true;
}

// What waits, each handled to its end, the port's first; then Lua is free
void lua_call_end(void) {
  sleep_handles = false;
  for (;;) {
    if (port_waiting) {
      port_waiting = false;
      handle(port);
    } else if (waiting > 0) {
      handle(take());
    } else {
      break;
    }
  }
  running = false;
}

void lua_event_arrived(LuaEvent e) {
  if (!running) {
    running = true;
    handle(e);
    lua_call_end();
  } else if (is_port(e)) {
    port_waiting = true;
  } else if (waiting < LINE) {
    line[(first + waiting) % LINE] = e;
    waiting++;
  }
}

void lua_events_sleep(uint32_t ms) {
  uint32_t start = lua_events_now(), elapsed;
  bool paused = false;
  if (!sleep_handles) {
    lua_events_pause(ms);
    return;
  }
  for (;;) {
    // the events that wait now: those that come while their handlers run
    // wait for the next look, so the sleep ends on time
    int n = waiting;
    while (n-- > 0)
      handle(take());
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
