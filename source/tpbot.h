// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef TPBOT_H
#define TPBOT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "lua.h"

// The tpbot module, as X(Lua name, C function): the TPBot Classic's
// commands, each sending one Classic frame and returning.
#define TPBOT_FUNCTIONS                                 \
    X(set_car_light,    tpbot_set_car_light)            \
    X(set_motors_speed, tpbot_set_motors_speed)         \
    X(info,             tpbot_info)                     \
    X(get_distance,     tpbot_get_distance)

// The tpbot2 module: the TPBot Edu's commands, each sending one Edu frame
// and returning, and its done flag.
#define TPBOT2_FUNCTIONS                                \
    X(set_car_light,    tpbot2_set_car_light)           \
    X(set_motors_speed, tpbot2_set_motors_speed)        \
    X(speed_control,    tpbot2_speed_control)           \
    X(set_speed,        tpbot2_set_speed)               \
    X(run_distance,     tpbot2_run_distance)            \
    X(turn,             tpbot2_turn)                    \
    X(pivot,            tpbot2_pivot)                   \
    X(done,             tpbot2_done)                    \
    X(set_servo,        tpbot2_set_servo)               \
    X(adjust_motor_speed, tpbot2_adjust_motor_speed)    \
    X(reset_motor_adjust, tpbot2_reset_motor_adjust)    \
    X(info,             tpbot_info)                     \
    X(get_distance,     tpbot_get_distance)

#define X(name, function) int function(lua_State *L);
TPBOT_FUNCTIONS
TPBOT2_FUNCTIONS
#undef X


// What a robot command says when no robot answers on the bus: tpbot's,
// tpbot2's and Nezha 2's
extern const char robot_no_answer[];

// What the commands need of the board, given by the firmware, or by the
// host test in its place.

// Write length bytes to the device at address on the I2C bus: 0 when it
// took them, anything else when it did not. data may be anywhere, a string
// literal in flash included.
int tpbot_i2c_write(int address, const char *data, size_t length);

// Read length bytes from the device at address: 0 when it gave them,
// anything else when it did not. A TPBot Classic that is read from holds
// the bus until it is switched off and on.
int tpbot_i2c_read(int address, char *data, size_t length);

// The sonar's echo in microseconds, or -1 when none came.
int tpbot_echo_us(void);

#ifdef __cplusplus
}
#endif

#endif
