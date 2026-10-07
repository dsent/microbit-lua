// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef TPBOT_H
#define TPBOT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "lua.h"

// The tpbot module's functions, as X(Lua name, C function).
#define TPBOT_FUNCTIONS                                 \
    X(set_car_light,    tpbot_set_car_light)            \
    X(set_motors_speed, tpbot_set_motors_speed)         \
    X(get_distance,     tpbot_get_distance)             \
    X(run_distance,     tpbot_run_distance)             \
    X(turn,             tpbot_turn)

// The robot commands that are globals.
#define TPBOT_GLOBALS                                   \
    X(robot_info,       tpbot_robot_info)               \
    X(robot_move,       tpbot_robot_move)               \
    X(turn,             tpbot_turn_hours)               \
    X(straight,         tpbot_straight)

#define X(name, function) int function(lua_State *L);
TPBOT_FUNCTIONS
TPBOT_GLOBALS
#undef X

// Sets the globals above.
void tpbot_register_globals(lua_State *L);

// What a robot command says when no robot answers on the bus
extern const char tpbot_no_answer[];

// What the commands need of the board, given by the firmware, or by the
// host test in its place.

// The most bytes tpbot_i2c_write takes in one frame; the longest frame a
// robot command writes is 9 bytes.
#define TPBOT_FRAME_MAX 16

// Write length bytes, at most TPBOT_FRAME_MAX, to the device at address
// on the I2C bus: 0 when it took them, anything else when it did not.
// data may be anywhere, a string literal in flash included.
int tpbot_i2c_write(int address, const char *data, size_t length);

// Sleep, letting other fibers run.
void tpbot_sleep(uint32_t ms);

// The sonar's echo in microseconds, or -1 when none came.
int tpbot_echo_us(void);

#ifdef __cplusplus
}
#endif

#endif
