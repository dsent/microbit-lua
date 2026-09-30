#ifndef CODAL_LUA_H
#define CODAL_LUA_H

extern "C" {
#include "lua.h"
}

// Lua 5.1 does not define LUA_OK; provide it so callers can write
// `== LUA_OK` instead of remembering that 0 is success.
#ifndef LUA_OK
#define LUA_OK 0
#endif

void register_lua_modules(lua_State *L);
void register_lua_event_listener(lua_State *L);

// A mistake, written to the serial port on a line of its own, what it is
// first ("Runtime error: "), with nothing taken from the heap
void port_mistake(const char *what, const char *message);

// Whether the serial port is the USB one, the Compy's console
extern bool port_is_console;

#include "lua-strip-debug.h"

#endif
