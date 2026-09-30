// -*- mode: c; indent-tabs-mode: nil; -*-

#include <string.h>

#include "lauxlib.h"
#include "link-words.h"

int link_words_not_sent(lua_State *L) {
  size_t len;
  const char *line = luaL_checklstring(L, 1, &len);
  // read before the buffer, whose pieces may take a missing one's place
  int typed_after = lua_toboolean(L, 2);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  luaL_addstring(&b, "\nThe other micro:bit did not answer, so it may not "
                 "have got: ");
  luaL_addlstring(&b, line, len);
  luaL_addchar(&b, '\n');
  if (typed_after)
    luaL_addstring(&b, "What you typed after it was not sent either.\n");
  luaL_addstring(&b, "Wait until the other micro:bit has finished. Then "
                 "press the reset button on the back of this micro:bit and "
                 "connect again. Check what ran, and type again, from its "
                 "first line, any statement that did not run.\n");
  luaL_pushresult(&b);
  return 1;
}

int link_words_typed(lua_State *L) {
  size_t have, len, at, n = 0, i;
  const char *sofar = luaL_checklstring(L, 1, &have);
  const char *text = luaL_checklstring(L, 2, &len);
  // the line, then what shows: each character typed shows as 3 at most
  char *line = (char *)lua_newuserdata(L, have + 4 * len);
  char *shown = line + have + len;
  memcpy(line, sofar, have);
  at = have;
  for (i = 0; i < len; i++) {
    char c = text[i];
    if (c == '\b' || c == 127) {
      if (at > 0 && line[at - 1] != '\r' && line[at - 1] != '\n') {
        at--;
        memcpy(shown + n, "\b \b", 3);
        n += 3;
      }
    } else {
      line[at++] = c;
      shown[n++] = c == '\r' ? '\n' : c;
    }
  }
  lua_pushlstring(L, line, at);
  lua_pushlstring(L, shown, n);
  return 2;
}
