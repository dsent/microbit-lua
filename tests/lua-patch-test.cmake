# cmake -DROOT=<repo> -DWORK=<empty dir> -P tests/lua-patch-test.cmake
#
# lua_patch (source/lua-patch.cmake) on a fresh Lua: every patch in
# CMakeLists.txt's list applied, then applied again with nothing changed;
# a file with an unrelated comment holding a patch's words still patched;
# and a patched block changed by hand an error, not a skip.
set(LUA_PATCH_DIR "${ROOT}/source")
include("${ROOT}/source/lua-patch.cmake")
set(LUA_SRC_DIR "${WORK}/lua-5.1.5/src")

file(READ "${ROOT}/CMakeLists.txt" _cmake)
string(REGEX MATCHALL "\nlua_patch\\([^ ]+ [^ )]+\\)" _calls "${_cmake}")
function(apply_all)
    foreach(_call IN LISTS _calls)
        string(REGEX REPLACE "\nlua_patch\\(([^ ]+) ([^ )]+)\\)" "\\1;\\2" _args "${_call}")
        lua_patch(${_args})
    endforeach()
endfunction()

function(fail WHAT)
    message(FATAL_ERROR "FAIL ${WHAT}")
endfunction()

function(fresh_lua)
    file(REMOVE_RECURSE "${WORK}/lua-5.1.5")
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf
                    "${ROOT}/libraries/lua-5.1.5.tar.gz"
                    WORKING_DIRECTORY "${WORK}")
endfunction()

list(LENGTH _calls _count)
fresh_lua()
apply_all()
file(GLOB _sources "${LUA_SRC_DIR}/*")
set(_before "")
foreach(_f IN LISTS _sources)
    file(SHA256 "${_f}" _h)
    string(APPEND _before "${_h}")
endforeach()
apply_all()
set(_after "")
foreach(_f IN LISTS _sources)
    file(SHA256 "${_f}" _h)
    string(APPEND _after "${_h}")
endforeach()
if(NOT _before STREQUAL _after)
    fail("the ${_count} patches applied twice change the sources the second time")
endif()
message(STATUS "ok   the ${_count} patches apply to a fresh Lua, and a second time change nothing")

# an unrelated comment with a patch's own words does not pass for it
fresh_lua()
file(APPEND "${LUA_SRC_DIR}/ldebug.c" "/* no line info */\n")
lua_patch(ldebug.c ldebug-no-line.patch)
file(READ "${LUA_SRC_DIR}/ldebug.c" _text)
string(FIND "${_text}" "luaO_pushfstring(L, \"%s: %s\", buff, msg);" _at)
if(_at EQUAL -1)
    fail("a comment holding \"no line info\" is taken for ldebug-no-line.patch")
endif()
message(STATUS "ok   a comment holding a patch's words does not pass for the patch")

# a patched block changed by hand is an error
string(REPLACE "luaO_pushfstring(L, \"%s: %s\", buff, msg);"
               "luaO_pushfstring(L, \"%s:0: %s\", buff, msg);" _text "${_text}")
file(WRITE "${LUA_SRC_DIR}/ldebug.c" "${_text}")
execute_process(
    COMMAND ${CMAKE_COMMAND} -DROOT=${ROOT} -DWORK=${WORK}
            -DLUA_SRC_DIR=${LUA_SRC_DIR}
            -P "${ROOT}/tests/lua-patch-one.cmake"
    RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
if(_rc EQUAL 0)
    fail("a patched block changed by hand is taken as patched")
endif()
message(STATUS "ok   a patched block changed by hand stops the build")
