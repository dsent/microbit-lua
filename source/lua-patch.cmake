# lua_patch(FILE PATCH): source/PATCH applied to FILE of the Lua sources in
# LUA_SRC_DIR, once. Patches go in place because lua.h includes luaconf.h by
# quoted include, so a shadow header cannot take precedence; libraries/
# survives a clean build, so a file may already be patched.
#
# A file counts as patched when it holds every line the patch adds, the
# patched block itself, so no word elsewhere in the file can pass for it.
# Otherwise the patch is applied, and a file that matches neither the
# patch's context nor its result is an error. LUA_PATCH_DIR is where the
# patches are.
function(lua_patch FILE PATCH)
    set(_file "${LUA_SRC_DIR}/${FILE}")
    set(_patch "${LUA_PATCH_DIR}/${PATCH}")
    file(READ "${_file}" _text)
    file(READ "${_patch}" _diff)
    # what a CMake list or a string would read specially, out of the way
    foreach(_text_or_diff _text _diff)
        string(REPLACE "\\" "<backslash>" ${_text_or_diff} "${${_text_or_diff}}")
        string(REPLACE ";" "<semicolon>" ${_text_or_diff} "${${_text_or_diff}}")
        string(REPLACE "[" "<open>" ${_text_or_diff} "${${_text_or_diff}}")
        string(REPLACE "]" "<close>" ${_text_or_diff} "${${_text_or_diff}}")
    endforeach()
    string(REGEX MATCHALL "\n\\+[^\n]*" _added "\n${_diff}")
    set(_lines 0)
    set(_missing 0)
    foreach(_line IN LISTS _added)
        if(_line MATCHES "^\n\\+\\+\\+ ")
            continue()
        endif()
        string(SUBSTRING "${_line}" 2 -1 _code)
        string(STRIP "${_code}" _stripped)
        if(_stripped STREQUAL "")
            continue()
        endif()
        math(EXPR _lines "${_lines} + 1")
        string(FIND "${_text}" "${_code}" _at)
        if(_at EQUAL -1)
            math(EXPR _missing "${_missing} + 1")
        endif()
    endforeach()
    if(_lines EQUAL 0)
        message(FATAL_ERROR "${PATCH} adds no lines")
    endif()
    if(_missing EQUAL 0)
        return()
    endif()
    execute_process(
        COMMAND patch --forward --batch "${_file}" "${_patch}"
        RESULT_VARIABLE _rc
    )
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "Failed to patch ${FILE} with ${PATCH} (${_rc})")
    endif()
endfunction()
