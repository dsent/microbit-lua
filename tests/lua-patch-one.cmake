# The build's Lua patches, as CMakeLists.txt lists them, applied to
# LUA_SRC_DIR: for tests/lua-patch-test.cmake, which runs this where the
# build must stop. PATCH_DIR, if given, holds the patches in source/'s
# place; SKIP, if given, is a patch left off the list.
if(PATCH_DIR)
    set(LUA_PATCH_DIR "${PATCH_DIR}")
else()
    set(LUA_PATCH_DIR "${ROOT}/source")
endif()
set(LUA_ARCHIVE "${ROOT}/libraries/lua-5.1.5.tar.gz")
include("${ROOT}/source/lua-patch.cmake")
file(READ "${ROOT}/CMakeLists.txt" _cmake)
string(REGEX MATCHALL "\nlua_patch\\([^ ]+ [^ )]+\\)" _calls "${_cmake}")
if(EXTRA)
    list(APPEND _calls "\nlua_patch(${EXTRA})")
endif()
foreach(_call IN LISTS _calls)
    string(REGEX REPLACE "\nlua_patch\\(([^ ]+) ([^ )]+)\\)" "\\1;\\2" _args "${_call}")
    list(GET _args 1 _patch)
    if(NOT _patch STREQUAL SKIP)
        lua_patch(${_args})
    endif()
endforeach()
lua_patch_apply()
