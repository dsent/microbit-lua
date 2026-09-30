# Patches to the Lua sources in LUA_SRC_DIR, from LUA_PATCH_DIR, against
# Lua as LUA_ARCHIVE ships it. They go in place because lua.h includes
# luaconf.h by quoted include, so a shadow header cannot take precedence;
# and libraries/ survives a clean build, so a file may hold them already, or
# some of them.
#
# lua_patch(FILE PATCH) lists a patch, in the order it goes in;
# lua_patch_apply() then works file by file. A file's patches go in one
# after another, and a later one may change the context of an earlier one,
# so a file is taken as holding the first J of its patches when a copy of it,
# with those taken out again last first, every hunk in place with its context
# (patch --reverse, no fuzz), is the file as Lua ships it; the rest go in
# after them. It looks for the most J that fits, down to none, and writes the
# file only once all of its patches fit. A file no J fits stops the build: one
# changed by hand, or holding a patch no longer listed, or a hunk an edited
# patch no longer has, with the way back, a fresh Lua; one as Lua ships it,
# with the patch that does not go in.

find_program(LUA_PATCH_TOOL patch REQUIRED)

function(lua_patch FILE PATCH)
    set_property(GLOBAL APPEND PROPERTY LUA_PATCHES "${FILE}|${PATCH}")
endfunction()

# patch, no fuzz, no backup or reject files; the result in _ok
function(_lua_patch_run FILE PATCH)
    execute_process(
        COMMAND "${LUA_PATCH_TOOL}" ${ARGN} --forward --batch --fuzz=0
                --silent --no-backup-if-mismatch --reject-file=-
                "${FILE}" "${PATCH}"
        RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
    if(_rc EQUAL 0)
        set(_ok TRUE PARENT_SCOPE)
    else()
        set(_ok FALSE PARENT_SCOPE)
    endif()
endfunction()

# whether two files hold the same bytes, in _same
function(_lua_patch_same A B)
    file(SHA256 "${A}" _a)
    file(SHA256 "${B}" _b)
    if(_a STREQUAL _b)
        set(_same TRUE PARENT_SCOPE)
    else()
        set(_same FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_lua_patch_file FILE PRISTINE)
    set(_file "${LUA_SRC_DIR}/${FILE}")
    set(_scratch "${LUA_SRC_DIR}/${FILE}.patching")
    list(LENGTH ARGN _k)
    set(_j ${_k})
    while(_j GREATER_EQUAL 0)
        # the first J taken out, last first, give Lua's own file
        configure_file("${_file}" "${_scratch}" COPYONLY)
        set(_ok TRUE)
        set(_i ${_j})
        while(_ok AND _i GREATER 0)
            math(EXPR _at "${_i} - 1")
            list(GET ARGN ${_at} _patch)
            _lua_patch_run("${_scratch}" "${LUA_PATCH_DIR}/${_patch}" --reverse)
            math(EXPR _i "${_i} - 1")
        endwhile()
        if(_ok)
            _lua_patch_same("${_scratch}" "${PRISTINE}")
            set(_ok ${_same})
        endif()
        if(_ok)
            # and the rest go in after them
            configure_file("${_file}" "${_scratch}" COPYONLY)
            set(_i ${_j})
            while(_ok AND _i LESS _k)
                list(GET ARGN ${_i} _patch)
                _lua_patch_run("${_scratch}" "${LUA_PATCH_DIR}/${_patch}")
                math(EXPR _i "${_i} + 1")
            endwhile()
            if(_ok)
                if(_j LESS _k)
                    file(RENAME "${_scratch}" "${_file}")
                else()
                    file(REMOVE "${_scratch}")
                endif()
                return()
            endif()
            file(REMOVE "${_scratch}")
            message(FATAL_ERROR
                "${_patch} does not go into ${FILE} after the patches "
                "before it: the patch no longer fits Lua 5.1.5 as it ships, "
                "and needs making again.")
        endif()
        math(EXPR _j "${_j} - 1")
    endwhile()
    file(REMOVE "${_scratch}")
    get_filename_component(_lua "${LUA_SRC_DIR}" DIRECTORY)
    string(REPLACE ";" ", " _names "${ARGN}")
    message(FATAL_ERROR
        "${FILE} in ${_lua} is neither as Lua ships it nor as its patches "
        "(${_names}) leave it: it was changed, or holds a patch no longer "
        "listed, or one edited since. Delete ${_lua}, and the next build "
        "unpacks a fresh Lua and patches it again.")
endfunction()

function(lua_patch_apply)
    get_property(_all GLOBAL PROPERTY LUA_PATCHES)
    set(_files "")
    foreach(_entry IN LISTS _all)
        string(REPLACE "|" ";" _pair "${_entry}")
        list(GET _pair 0 _file)
        list(GET _pair 1 _patch)
        list(APPEND _files "${_file}")
        if(NOT EXISTS "${LUA_PATCH_DIR}/${_patch}")
            message(FATAL_ERROR "The Lua patch ${_patch} is listed, but "
                    "${LUA_PATCH_DIR} has no such file.")
        endif()
        # an edited patch reconfigures the build
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${LUA_PATCH_DIR}/${_patch}")
    endforeach()
    list(REMOVE_DUPLICATES _files)
    # Lua's own files, to tell a patched file from one changed otherwise
    get_filename_component(_lua "${LUA_SRC_DIR}" DIRECTORY)
    set(_pristine "${_lua}.pristine")
    file(REMOVE_RECURSE "${_pristine}")
    file(MAKE_DIRECTORY "${_pristine}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf "${LUA_ARCHIVE}"
        WORKING_DIRECTORY "${_pristine}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${LUA_ARCHIVE} could not be unpacked: delete "
                "it, and the next build downloads it again.")
    endif()
    foreach(_file IN LISTS _files)
        set(_patches "")
        foreach(_entry IN LISTS _all)
            string(REPLACE "|" ";" _pair "${_entry}")
            list(GET _pair 0 _f)
            list(GET _pair 1 _p)
            if(_f STREQUAL _file)
                list(APPEND _patches "${_p}")
            endif()
        endforeach()
        _lua_patch_file("${_file}"
                        "${_pristine}/lua-5.1.5/src/${_file}" ${_patches})
    endforeach()
    file(REMOVE_RECURSE "${_pristine}")
endfunction()
