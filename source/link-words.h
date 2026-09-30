// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef LINK_WORDS_H
#define LINK_WORDS_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

// microbit.radio.notSent(line, typedAfter) -> string
// What the REPL says when a line over a radio link did not go: kept in C,
// so that its words take flash and no Lua heap. typedAfter: more was typed
// after the line, and dropped with it.
int link_words_not_sent(lua_State *L);

// microbit.radio.typed(sofar, text) -> line, shown
// Typing over a radio link, taken as the console takes it: text typed
// after sofar makes line, Backspace taking back the last character of a
// line not yet ended; shown is what the port shows of it, a line ending
// as one.
int link_words_typed(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif
