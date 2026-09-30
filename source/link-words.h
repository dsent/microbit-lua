// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef LINK_WORDS_H
#define LINK_WORDS_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

// microbit.radio.notSent(line, typedAfter, statementOpen) -> string
// What the REPL says when a line over a radio link did not go: kept in C,
// so that its words take flash and no Lua heap. typedAfter: more was typed
// after the line, and dropped with it; statementOpen: the other board
// last showed ">> ".
int link_words_not_sent(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif
