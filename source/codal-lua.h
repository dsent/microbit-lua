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

extern "C" void lua_strip_debug(lua_State *L);

#endif
