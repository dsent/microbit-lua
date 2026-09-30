// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef LUA_STRIP_DEBUG_H
#define LUA_STRIP_DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lua.h"

// The function on top of the stack, a chunk just loaded, stripped of its
// line info and local and upvalue names (source/lua-strip-debug.c)
void lua_strip_debug(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif
