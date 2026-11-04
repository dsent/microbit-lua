# cmake -DROOT=<repo> -DWORK=<empty dir> -P tests/lua-patch-test.cmake
#
# The build's Lua patches (source/lua-patch.cmake), on Lua sources in each
# state libraries/ can hold: fresh, patched as plain patch patches them;
# patched already, left as they are; patched in part, finished. And the
# build stopping, naming the way back, at a file changed otherwise: with a
# comment holding a patch's words, at a patched block that has lost a line
# the file holds elsewhere, at a patch no longer listed, and at a patch
# edited since, a hunk dropped or a line changed; and naming the patch at
# one that no longer fits Lua as it ships, or that is missing, or with no
# patch tool. A file no patch touches, changed, stops the build too. And
# the Lua tarball: lua.org's goes on, another stops the build.

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
# ARGN: more -D settings for tests/lua-patch-one.cmake
function(apply DIR)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -DROOT=${ROOT}
                -DLUA_SRC_DIR=${WORK}/${DIR}/lua-5.1.5/src ${ARGN}
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

# a comment holding the new block's words does not pass for it: the file is
# not Lua's own, and stops the build
fresh_lua(comment)
file(APPEND "${WORK}/comment/lua-5.1.5/src/luaconf.h"
     "/* #if defined(LUA_NUMBER_IS_FLOAT) && LUA_NUMBER_IS_FLOAT */\n")
apply(comment)
string(FIND "${_said}" "Delete" _named)
check("an unpatched luaconf.h whose comments hold the patch's words stops the build" NOT _rc EQUAL 0 AND NOT _named EQUAL -1)

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
damaged(luaconf.h "#define lua_number2str(s,n)\tsprintf((s), LUA_NUMBER_FMT, (n))\n" 0
        "a luaconf.h whose patched block lost a line stops the build, naming the way back")

# the fully patched Lua in WORK/<dir>
function(patched_copy DIR)
    file(REMOVE_RECURSE "${WORK}/${DIR}")
    file(COPY "${WORK}/build/lua-5.1.5" DESTINATION "${WORK}/${DIR}")
endfunction()

# the build stopped, its words holding WORDS
function(stopped WORDS WHAT)
    # CMake wraps a message where the paths in it make it long
    string(REGEX REPLACE "[ \n]+" " " _said "${_said}")
    string(FIND "${_said}" "${WORDS}" _named)
    check("${WHAT}" NOT _rc EQUAL 0 AND NOT _named EQUAL -1)
endfunction()

# a patch left off the list, still in the file
patched_copy(unlisted)
apply(unlisted -DSKIP=luaconf-float.patch)
stopped("Delete" "a luaconf.h holding a patch no longer listed stops the build")

# patches edited since: a hunk dropped, a line changed
file(REMOVE_RECURSE "${WORK}/edited")
file(COPY "${ROOT}/source/" DESTINATION "${WORK}/edited")
file(READ "${WORK}/edited/luaconf-float.patch" _diff)
string(FIND "${_diff}" "\n@@ " _first)
math(EXPR _from "${_first} + 1")
string(SUBSTRING "${_diff}" ${_from} -1 _after)
string(FIND "${_after}" "\n@@ " _second)
math(EXPR _keep "${_from} + ${_second} + 1")
string(SUBSTRING "${_diff}" 0 ${_keep} _diff)
file(WRITE "${WORK}/edited/luaconf-float.patch" "${_diff}")
patched_copy(hunk)
apply(hunk -DPATCH_DIR=${WORK}/edited)
stopped("Delete" "a patch edited down to its first hunk stops the build")
file(COPY "${ROOT}/source/luaconf-float.patch" DESTINATION "${WORK}/edited")
file(READ "${WORK}/edited/luaconf-float.patch" _diff)
string(REPLACE "\"%.9g\"" "\"%.8g\"" _diff "${_diff}")
file(WRITE "${WORK}/edited/luaconf-float.patch" "${_diff}")
patched_copy(words)
apply(words -DPATCH_DIR=${WORK}/edited)
stopped("Delete" "a patch whose words were edited since stops the build")

# a patch that does not fit Lua as it ships: named, with no advice to
# delete what is not at fault
file(COPY "${ROOT}/source/luaconf-float.patch" DESTINATION "${WORK}/edited")
file(READ "${WORK}/edited/luaconf-float.patch" _diff)
string(REPLACE "-#define LUA_NUMBER_SCAN\t\t\"%lf\""
               "-#define LUA_NUMBER_SCAN\t\t\"%Lf\""
               _diff "${_diff}")
file(WRITE "${WORK}/edited/luaconf-float.patch" "${_diff}")
fresh_lua(unfit)
apply(unfit -DPATCH_DIR=${WORK}/edited)
stopped("luaconf-float.patch does not go into luaconf.h"
        "a patch that no longer fits Lua as it ships is named")

# a file no patch touches, changed, or gone
patched_copy(untouched)
file(APPEND "${WORK}/untouched/lua-5.1.5/src/lvm.c" "/* changed */\n")
apply(untouched)
stopped("lvm.c in" "a file no patch touches, changed, stops the build")
patched_copy(untouched_gone)
file(REMOVE "${WORK}/untouched_gone/lua-5.1.5/src/lgc.c")
apply(untouched_gone)
stopped("lgc.c in" "... and so does one gone")

# a listed patch missing, and no patch tool
fresh_lua(missing)
apply(missing "-DEXTRA=ldo.c no-such.patch")
stopped("has no such file" "a listed patch that is missing is named")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env PATH=${WORK}/nowhere
            ${CMAKE_COMMAND} -DROOT=${ROOT}
            -DLUA_SRC_DIR=${WORK}/missing/lua-5.1.5/src
            -P "${ROOT}/tests/lua-patch-one.cmake"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
set(_said "${_out}${_err}")
stopped("LUA_PATCH_TOOL" "with no patch tool the build says so")

# the tarball, by the SHA256 CMakeLists.txt gives it: lua.org's goes on,
# anything else stops the build, naming the way back
string(REGEX MATCH "set\\(LUA_ARCHIVE_SHA256[ \n]+([0-9a-f]+)\\)" _m "${_cmake}")
set(_sha256 "${CMAKE_MATCH_1}")
file(WRITE "${WORK}/archive-check.cmake"
     "include(\"${ROOT}/source/lua-patch.cmake\")\nlua_archive_check()\n")
function(archive ARCHIVE)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -DLUA_ARCHIVE=${ARCHIVE}
                -DLUA_ARCHIVE_SHA256=${_sha256}
                -P "${WORK}/archive-check.cmake"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(_rc ${_rc} PARENT_SCOPE)
    set(_said "${_out}${_err}" PARENT_SCOPE)
endfunction()
archive("${ROOT}/libraries/lua-5.1.5.tar.gz")
check("lua.org's tarball goes on" _rc EQUAL 0 AND _sha256 MATCHES "^[0-9a-f]+$")
file(WRITE "${WORK}/not-lua.tar.gz" "not Lua")
archive("${WORK}/not-lua.tar.gz")
stopped("is not the Lua lua.org ships" "a tarball that is not lua.org's stops the build")
