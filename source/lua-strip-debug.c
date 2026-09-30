// -*- mode: c; indent-tabs-mode: nil; -*-
//
// Strip debug information from a freshly loaded Lua chunk.
//
// Parsing text always makes Lua allocate line info, local-variable names and
// upvalue names for every function prototype. The debug library is not opened
// on this device and the embedded chunk is never traced, so those arrays are
// pure RAM overhead once the chunk has been loaded. Freeing them keeps the
// behaviour identical while returning roughly as much RAM as the bytecode's
// debug section would have cost.
//
// Call this with the loaded function on top of the stack, before running it.
// It walks the functions nested in it a C call deeper each, with no check
// of the C stack: the chunk was parsed from text at the same depth, and each
// level of nesting took the parser, which checks, far more stack than it
// takes here.

#include <stddef.h>

#include "lua.h"
#include "lua-strip-debug.h"
#include "lobject.h"
#include "lmem.h"

static void lua_strip_proto(lua_State *L, Proto *f) {
    int i;

    if (f->lineinfo != NULL) {
        luaM_freearray(L, f->lineinfo, f->sizelineinfo, int);
        f->lineinfo = NULL;
        f->sizelineinfo = 0;
    }
    if (f->locvars != NULL) {
        luaM_freearray(L, f->locvars, f->sizelocvars, LocVar);
        f->locvars = NULL;
        f->sizelocvars = 0;
    }
    if (f->upvalues != NULL) {
        luaM_freearray(L, f->upvalues, f->sizeupvalues, TString *);
        f->upvalues = NULL;
        f->sizeupvalues = 0;
    }

    for (i = 0; i < f->sizep; i++) {
        lua_strip_proto(L, f->p[i]);
    }
}

void lua_strip_debug(lua_State *L) {
    const Closure *cl = (const Closure *)lua_topointer(L, -1);

    if (cl != NULL && !cl->c.isC) {
        lua_strip_proto(L, cl->l.p);
    }
}
