# cmake -DROOT=<repo> -DWORK=<empty dir> -P tests/lua-patch-test.cmake
#
# The build's Lua patches (source/lua-patch.cmake), on Lua sources in each
# state libraries/ can hold: fresh, patched as plain patch patches them;
# patched already, left as they are; patched in part, finished; with a
# comment holding a patch's words, patched all the same; and with a line of
# a patched block gone, though the same line is elsewhere in the file, or
# with an edited patch's words, stopping the build and naming the way back.

set(_patched "${WORK}/patched/lua-5.1.5/src")

# a fresh Lua in WORK/<dir>
function(fresh_lua DIR)
    file(REMOVE_RECURSE "${WORK}/${DIR}")
    file(MAKE_DIRECTORY "${WORK}/${DIR}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf
                    "${ROOT}/libraries/lua-5.1.5.tar.gz"
                    WORKING_DIRECTORY "${WORK}/${DIR}")
endfunction()

# the build's patches applied to WORK/<dir>; its exit code in _rc, what it
# said in _said
function(apply DIR)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -DROOT=${ROOT}
                -DLUA_SRC_DIR=${WORK}/${DIR}/lua-5.1.5/src
                -P "${ROOT}/tests/lua-patch-one.cmake"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(_rc ${_rc} PARENT_SCOPE)
    set(_said "${_out}${_err}" PARENT_SCOPE)
endfunction()

# whether WORK/<dir>'s sources are the same as those plain patch made
function(same DIR)
    file(GLOB _sources RELATIVE "${_patched}" "${_patched}/*")
    set(_same TRUE)
    foreach(_f IN LISTS _sources)
        file(SHA256 "${_patched}/${_f}" _a)
        if(EXISTS "${WORK}/${DIR}/lua-5.1.5/src/${_f}")
            file(SHA256 "${WORK}/${DIR}/lua-5.1.5/src/${_f}" _b)
        else()
            set(_b none)
        endif()
        if(NOT _a STREQUAL _b)
            set(_same FALSE)
        endif()
    endforeach()
    set(_same ${_same} PARENT_SCOPE)
endfunction()

# check(WHAT <condition>...)
function(check WHAT)
    if(${ARGN})
        message(STATUS "ok   ${WHAT}")
    else()
        message(FATAL_ERROR "FAIL ${WHAT}")
    endif()
endfunction()

# plain patch, in the build's order, for comparison
fresh_lua(patched)
file(READ "${ROOT}/CMakeLists.txt" _cmake)
string(REGEX MATCHALL "\nlua_patch\\([^ ]+ [^ )]+\\)" _calls "${_cmake}")
list(LENGTH _calls _count)
foreach(_call IN LISTS _calls)
    string(REGEX REPLACE "\nlua_patch\\(([^ ]+) ([^ )]+)\\)" "\\1" _f "${_call}")
    string(REGEX REPLACE "\nlua_patch\\(([^ ]+) ([^ )]+)\\)" "\\2" _p "${_call}")
    execute_process(COMMAND patch --forward --batch --silent
                    "${_patched}/${_f}" "${ROOT}/source/${_p}"
                    RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "plain patch refused ${_p}")
    endif()
endforeach()

fresh_lua(build)
apply(build)
same(build)
check("the ${_count} patches go into a fresh Lua as plain patch puts them" _rc EQUAL 0 AND _same)
apply(build)
same(build)
check("... and a second time change nothing" _rc EQUAL 0 AND _same)

# patched in part: luaconf.h holding its first two patches only
fresh_lua(part)
foreach(_p luaconf-float.patch luaconf-number-text.patch)
    execute_process(COMMAND patch --forward --batch --silent
                    "${WORK}/part/lua-5.1.5/src/luaconf.h" "${ROOT}/source/${_p}")
endforeach()
apply(part)
same(part)
check("a luaconf.h holding its first two patches gets the rest" _rc EQUAL 0 AND _same)

# a comment holding the new block's words does not pass for it
fresh_lua(comment)
file(APPEND "${WORK}/comment/lua-5.1.5/src/ldebug.c"
     "/* no line info */\n/* luaO_pushfstring(L, \"%s: %s\", buff, msg); */\n")
apply(comment)
file(READ "${WORK}/comment/lua-5.1.5/src/ldebug.c" _text)
string(FIND "${_text}" "    else  /* no line info: the chunk was stripped */" _at)
check("an ldebug.c whose comments hold the patch's words is patched" _rc EQUAL 0 AND NOT _at EQUAL -1)

# a line gone from a patched block, the same line standing elsewhere
function(damaged FILE LINE NTH WHAT)
    file(REMOVE_RECURSE "${WORK}/damaged")
    file(COPY "${WORK}/build/lua-5.1.5" DESTINATION "${WORK}/damaged")
    set(_path "${WORK}/damaged/lua-5.1.5/src/${FILE}")
    file(READ "${_path}" _text)
    # drop the NTH (0-based) occurrence of LINE
    set(_head "")
    set(_rest "${_text}")
    foreach(_n RANGE ${NTH})
        string(FIND "${_rest}" "${LINE}" _at)
        if(_at EQUAL -1)
            message(FATAL_ERROR "no ${LINE} to take out of ${FILE}")
        endif()
        string(SUBSTRING "${_rest}" 0 ${_at} _before)
        string(LENGTH "${LINE}" _len)
        math(EXPR _after "${_at} + ${_len}")
        string(SUBSTRING "${_rest}" ${_after} -1 _rest)
        if(_n EQUAL NTH)
            string(APPEND _head "${_before}")
        else()
            string(APPEND _head "${_before}${LINE}")
        endif()
    endforeach()
    file(WRITE "${_path}" "${_head}${_rest}")
    apply(damaged)
    string(FIND "${_said}" "Delete" _named)
    check("${WHAT}" NOT _rc EQUAL 0 AND NOT _named EQUAL -1)
endfunction()
damaged(lauxlib.c "      return;\n" 1
        "a luaL_where whose new return; is gone stops the build, naming the way back")
damaged(ldo.c "    case 2: luaD_throw(L, LUA_ERRERR);\n" 1
        "a resume whose C stack case 2 is gone stops the build")
