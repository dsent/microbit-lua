// -*- mode: c; indent-tabs-mode: nil; -*-

#include "lauxlib.h"
#include "link-words.h"

int link_words_not_sent(lua_State *L) {
  size_t len;
  const char *line = luaL_checklstring(L, 1, &len);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  luaL_addstring(&b, "\nThe other micro:bit did not answer, so it may not "
                 "have got: ");
  luaL_addlstring(&b, line, len);
  luaL_addchar(&b, '\n');
  if (lua_toboolean(L, 2))
    luaL_addstring(&b, "What you typed after it was not sent either.\n");
  luaL_addstring(&b, lua_toboolean(L, 3)
                 ? "Type ) and press Enter, then the whole statement again.\n"
                 : "It may still be running a command. Once it has "
                   "finished, check whether the line ran before you type "
                   "it again.\n");
  luaL_pushresult(&b);
  return 1;
}
