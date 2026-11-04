// -*- mode: c; indent-tabs-mode: nil; -*-
//
// How full the C stack is, for Lua (luai_cstack in luaconf.h).
//
// Every fiber runs in the one 8 KB stack region (__StackSize), and nothing
// guards its bottom: a call past it writes over the top of the heap, and
// the board stops with a 045 panic at the next fiber switch. Lua's own
// limit, LUAI_MAXCCALLS, allows 200 nested C calls, and on this build one
// takes 140 to 430 bytes (a metamethod, a pcall), so a recursion through
// C reaches the bottom long before it. luaD_call and each level of the
// parser ask luai_cstack first, and stop with "C stack overflow" when less
// than LUA_CSTACK_MARGIN is left.
//
// LUA_CSTACK_MARGIN, 2,304 bytes (5,888 in use at 8 KB), is what can be
// used below one check before the next, from the ARM build's
// -fstack-usage: one Lua call (luaD_call, luaD_precall, luaV_execute and
// luaD_precall again, 216 bytes), the deepest C function below it,
// string.format stopping on a bad argument with the 256-byte luaL_Buffer
// (1,584 bytes), and an interrupt with its exception frame (about 500
// bytes).

#include <stdint.h>

#include "luaconf.h"
#include "stack-probe.h"

#define LUA_CSTACK_MARGIN 2304

int luai_cstack(void) {
  return stack_probe_current() + LUA_CSTACK_MARGIN > stack_probe_region();
}
