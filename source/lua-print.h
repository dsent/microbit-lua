// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef LUA_PRINT_H
#define LUA_PRINT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "lua.h"

// Sets the global print, which writes to the serial port.
void lua_print_open(lua_State *L);

// Supplied by the firmware, or by the host test in its place: text out of
// the serial port.
void lua_print_out(const char *text, size_t length);

#ifdef __cplusplus
}
#endif

#endif
