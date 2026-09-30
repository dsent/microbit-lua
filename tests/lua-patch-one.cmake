# The build's Lua patches, as CMakeLists.txt lists them, applied to
# LUA_SRC_DIR: for tests/lua-patch-test.cmake, which runs this where the
# build must stop.
set(LUA_PATCH_DIR "${ROOT}/source")
include("${ROOT}/source/lua-patch.cmake")
file(READ "${ROOT}/CMakeLists.txt" _cmake)
string(REGEX MATCHALL "\nlua_patch\\([^ ]+ [^ )]+\\)" _calls "${_cmake}")
foreach(_call IN LISTS _calls)
    string(REGEX REPLACE "\nlua_patch\\(([^ ]+) ([^ )]+)\\)" "\\1;\\2" _args "${_call}")
    lua_patch(${_args})
endforeach()
lua_patch_apply()
