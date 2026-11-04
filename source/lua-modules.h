// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef LUA_MODULES_H
#define LUA_MODULES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lua.h"

// One entry per public name of an API namespace: a method (func set) or an
// event constant (func NULL, value set). Kept in flash; the namespace tables
// reference these arrays and materialise entries on first access.
typedef struct {
  const char *name;
  lua_CFunction func;
  lua_Integer value;
} LuaApi;

// A module require() can make: its name, its API, and what else its first
// require does, if anything.
typedef struct {
  const char *name;
  const LuaApi *api;
  void (*open)(lua_State *L);
} LuaModule;

// Sets package, module and require, over modules, a list ended by a NULL
// name that lives as long as the state.
void lua_modules_open(lua_State *L, const LuaModule *modules);

#ifdef __cplusplus
}
#endif

#endif
