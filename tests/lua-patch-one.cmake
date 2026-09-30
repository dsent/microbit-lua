# ldebug-no-line.patch to LUA_SRC_DIR's ldebug.c, for tests/lua-patch-test.cmake
set(LUA_PATCH_DIR "${ROOT}/source")
include("${ROOT}/source/lua-patch.cmake")
lua_patch(ldebug.c ldebug-no-line.patch)
