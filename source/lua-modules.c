// -*- mode: c; indent-tabs-mode: nil; -*-
//
// package, module and require.
//
// Nothing is there before the script runs but these three. require(name)
// makes a module from the firmware's list the first time it is asked for:
// its table is found or made where module() would put it, so
// require("microbit.audio") lands in the global microbit.audio, making a
// plain microbit table on the way if there is none yet, and returns it.
// The table gets a lazy __index over the module's API and nothing else: the
// _M, _NAME and _PACKAGE module() adds would cost heap in every table, and
// nothing reads them. A parent made on the way (microbit, for
// microbit.audio) is a plain table; requiring it later gives that same
// table its API. package.loaded is the registry's _LOADED, which module()
// relies on.

#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lua-modules.h"

static const LuaModule *modules;

// Resolve a missing field on an API namespace table. Upvalue 1 is the
// namespace's LuaApi array. The first matching entry is materialised (a C
// closure for a method, an integer for a constant) and cached in the table, so
// only names that are actually used ever allocate.
static int l_lazy_index(lua_State *L) {
  const char *key = (lua_type(L, 2) == LUA_TSTRING) ? lua_tostring(L, 2) : NULL;
  if (key != NULL) {
    const LuaApi *e = (const LuaApi *)lua_touserdata(L, lua_upvalueindex(1));
    for (; e->name != NULL; e++) {
      if (strcmp(e->name, key) == 0) {
        if (e->func != NULL)
          lua_pushcfunction(L, e->func);
        else
          lua_pushinteger(L, e->value);
        lua_pushvalue(L, 2);    // key
        lua_pushvalue(L, -2);   // value
        lua_rawset(L, 1);       // table[key] = value (cache it)
        return 1;
      }
    }
  }
  return 0;
}

// Give the table at absolute index idx the lazy __index over api.
static void lua_set_lazy_index(lua_State *L, int idx, const LuaApi *api) {
  lua_newtable(L);
  lua_pushlightuserdata(L, (void *)api);
  lua_pushcclosure(L, l_lazy_index, 1);
  lua_setfield(L, -2, "__index");
  lua_setmetatable(L, idx);
}


// module(), from lua-5.1.5/src/loadlib.c

static void setfenv (lua_State *L) {
  lua_Debug ar;
  if (lua_getstack(L, 1, &ar) == 0 ||
      lua_getinfo(L, "f", &ar) == 0 ||  /* get calling function */
      lua_iscfunction(L, -1))
    luaL_error(L, LUA_QL("module") " not called from a Lua function");
  lua_pushvalue(L, -2);
  lua_setfenv(L, -2);
  lua_pop(L, 1);
}


static void dooptions (lua_State *L, int n) {
  int i;
  for (i = 2; i <= n; i++) {
    lua_pushvalue(L, i);  /* get option (a function) */
    lua_pushvalue(L, -2);  /* module */
    lua_call(L, 1, 0);
  }
}


static void modinit (lua_State *L, const char *modname) {
  const char *dot;
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "_M");  /* module._M = module */
  lua_pushstring(L, modname);
  lua_setfield(L, -2, "_NAME");
  dot = strrchr(modname, '.');  /* look for last dot in module name */
  if (dot == NULL) dot = modname;
  else dot++;
  /* set _PACKAGE as package name (full module name minus last part) */
  lua_pushlstring(L, modname, dot - modname);
  lua_setfield(L, -2, "_PACKAGE");
}


/* push the module's table, creating it (and _LOADED[modname]) if needed,
   with no fields of its own */
static void push_module_table (lua_State *L, const char *modname) {
  lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
  lua_getfield(L, -1, modname);  /* get _LOADED[modname] */
  if (!lua_istable(L, -1)) {  /* not found? */
    lua_pop(L, 1);  /* remove previous result */
    /* try global variable (and create one if it does not exist) */
    if (luaL_findtable(L, LUA_GLOBALSINDEX, modname, 1) != NULL)
      luaL_error(L, "name conflict for module " LUA_QS, modname);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, modname);  /* _LOADED[modname] = new table */
  }
  lua_remove(L, -2);  /* remove _LOADED */
}


/* push the module's table as push_module_table does, initialised */
static void push_module (lua_State *L, const char *modname) {
  push_module_table(L, modname);
  /* check whether table already has a _NAME field */
  lua_getfield(L, -1, "_NAME");
  if (!lua_isnil(L, -1))  /* is table an initialized module? */
    lua_pop(L, 1);
  else {  /* no; initialize it */
    lua_pop(L, 1);
    modinit(L, modname);
  }
}


static int ll_module (lua_State *L) {
  const char *modname = luaL_checkstring(L, 1);
  int n = lua_gettop(L);  /* number of arguments */
  push_module(L, modname);
  lua_pushvalue(L, -1);
  setfenv(L);
  dooptions(L, n);
  return 0;
}



static int l_require(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  const LuaModule *m = modules;
  lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
  lua_getfield(L, -1, name);
  if (lua_toboolean(L, -1)) return 1;
  while (m->name != NULL && strcmp(m->name, name) != 0) m++;
  if (m->name == NULL)
    return luaL_error(L, "module " LUA_QS " not found", name);
  push_module_table(L, name);
  lua_set_lazy_index(L, lua_gettop(L), m->api);
  if (m->open != NULL)
    m->open(L);
  return 1;
}

void lua_modules_open(lua_State *L, const LuaModule *list) {
  modules = list;
  lua_newtable(L);                                    // package
  lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
  lua_setfield(L, -2, "loaded");
  lua_setglobal(L, "package");
  lua_register(L, "module", ll_module);
  lua_register(L, "require", l_require);
}
