# Writes the Lua payload the firmware embeds: the shared script with the
# robot library put in place of its --@ROBOT_LIBRARY@ line.
#
#   cmake -DSCRIPT=<shared script> -DLIBRARY=<robot library> \
#         -DPAYLOAD=<output> -P utils/cmake/lua-payload.cmake

set(MARKER "--@ROBOT_LIBRARY@\n")

file(READ "${SCRIPT}" script)
file(READ "${LIBRARY}" library)

string(FIND "${script}" "${MARKER}" at)
if(at EQUAL -1)
    message(FATAL_ERROR
        "${SCRIPT} has no --@ROBOT_LIBRARY@ line for ${LIBRARY}")
endif()

string(REPLACE "${MARKER}" "${library}" payload "${script}")
file(WRITE "${PAYLOAD}" "${payload}")
