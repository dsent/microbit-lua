// -*- mode: c; indent-tabs-mode: nil; -*-
//
// print, to the serial port.
//
// Lua's own print writes to stdout, which goes nowhere on this board, so
// a program that is the board's whole script would print nothing. This
// print writes its values as Lua's does, each through tostring and apart
// by tabs, with "\r\n" for each line end, as a terminal on the port
// expects. tostring is the one there at start, so a program that changes
// the global changes nothing here. A value that cannot be written is a
// mistake of the program's, raised as Lua's print raises it; every call
// into Lua is protected by the program's own.

#include "lua.h"
#include "lauxlib.h"
#include "lua-print.h"

static void out(const char *text, size_t length) {
  size_t from = 0, i;
  for (i = 0; i < length; i++) {
    if (text[i] == '\n') {
      if (i > from)
        lua_print_out(text + from, i - from);
      lua_print_out("\r\n", 2);
      from = i + 1;
    }
  }
  if (length > from)
    lua_print_out(text + from, length - from);
}

static int print(lua_State *L) {
  int n = lua_gettop(L), i;
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  for (i = 1; i <= n; i++) {
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, i);
    lua_call(L, 1, 1);
    if (!lua_isstring(L, -1))
      return luaL_error(L, "'tostring' must return a string to 'print'");
    if (i > 1)
      luaL_addchar(&b, '\t');
    luaL_addvalue(&b);
  }
  luaL_addchar(&b, '\n');
  luaL_pushresult(&b);
  {
    size_t length;
    const char *text = lua_tolstring(L, -1, &length);
    out(text, length);
  }
  return 0;
}

void lua_print_open(lua_State *L) {
  lua_getglobal(L, "tostring");
  lua_pushcclosure(L, print, 1);
  lua_setglobal(L, "print");
}
