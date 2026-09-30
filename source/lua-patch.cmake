# Patches to the Lua sources in LUA_SRC_DIR, from LUA_PATCH_DIR. They go in
# place because lua.h includes luaconf.h by quoted include, so a shadow
# header cannot take precedence; and libraries/ survives a clean build, so a
# file may hold them already, or some of them.
#
# lua_patch(FILE PATCH) lists a patch, in the order it goes in;
# lua_patch_apply() then works file by file. A file's patches go in one
# after another, and a later one may change the context of an earlier one,
# so a file is taken as holding the first J of its patches when a copy of it,
# with those taken out again last first, every hunk in place with its context
# (patch --reverse, no fuzz), gives the file back as the first went into; the
# rest go in after them. It looks for the most J that fits, down to none, and
# writes the file only once all of its patches fit. A file no J fits, changed
# by hand or by a patch edited since, stops the build, naming the way back: a
# fresh Lua.

function(lua_patch FILE PATCH)
    set_property(GLOBAL APPEND PROPERTY LUA_PATCHES "${FILE}|${PATCH}")
endfunction()

# patch, no fuzz, no backup or reject files; the result in _ok
function(_lua_patch_run FILE PATCH)
    execute_process(
        COMMAND patch ${ARGN} --forward --batch --fuzz=0 --silent
                --no-backup-if-mismatch --reject-file=- "${FILE}" "${PATCH}"
        RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
    if(_rc EQUAL 0)
        set(_ok TRUE PARENT_SCOPE)
    else()
        set(_ok FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_lua_patch_file FILE)
    set(_file "${LUA_SRC_DIR}/${FILE}")
    set(_scratch "${LUA_SRC_DIR}/${FILE}.patching")
    list(LENGTH ARGN _k)
    foreach(_patch IN LISTS ARGN)
        # an edited patch reconfigures the build
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${LUA_PATCH_DIR}/${_patch}")
    endforeach()
    set(_j ${_k})
    while(_j GREATER_EQUAL 0)
        configure_file("${_file}" "${_scratch}" COPYONLY)
        set(_ok TRUE)
        # the first J taken out, last first
        set(_i ${_j})
        while(_ok AND _i GREATER 0)
            math(EXPR _at "${_i} - 1")
            list(GET ARGN ${_at} _patch)
            _lua_patch_run("${_scratch}" "${LUA_PATCH_DIR}/${_patch}" --reverse)
            math(EXPR _i "${_i} - 1")
        endwhile()
        if(_ok)
            # and all of them put back, then the rest
            configure_file("${_file}" "${_scratch}" COPYONLY)
            set(_i ${_j})
            while(_ok AND _i LESS _k)
                list(GET ARGN ${_i} _patch)
                _lua_patch_run("${_scratch}" "${LUA_PATCH_DIR}/${_patch}")
                math(EXPR _i "${_i} + 1")
            endwhile()
        endif()
        if(_ok)
            if(_j LESS _k)
                file(COPY_FILE "${_scratch}" "${_file}")
            endif()
            file(REMOVE "${_scratch}")
            return()
        endif()
        math(EXPR _j "${_j} - 1")
    endwhile()
    file(REMOVE "${_scratch}")
    get_filename_component(_lua "${LUA_SRC_DIR}" DIRECTORY)
    string(REPLACE ";" ", " _names "${ARGN}")
    message(FATAL_ERROR
        "${FILE} in ${_lua} is neither as Lua ships it nor as its patches "
        "(${_names}) leave it: it was changed, or a patch was. Delete "
        "${_lua}, and the next build unpacks a fresh Lua and patches it "
        "again.")
endfunction()

function(lua_patch_apply)
    get_property(_all GLOBAL PROPERTY LUA_PATCHES)
    set(_files "")
    foreach(_entry IN LISTS _all)
        string(REPLACE "|" ";" _pair "${_entry}")
        list(GET _pair 0 _file)
        list(APPEND _files "${_file}")
    endforeach()
    list(REMOVE_DUPLICATES _files)
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
        _lua_patch_file("${_file}" ${_patches})
    endforeach()
endfunction()
