// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef LUA_CSTACK_H
#define LUA_CSTACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The C stack limits, each the stack region's size less a margin, so that
// they follow __StackSize (nrf52833.ld.patch and its softdevice twin); the
// firmware reads the region's size at run time (stack_probe_region). At
// 8 KB they are 5,888, 6,400 and 4,352 bytes. The margins' account is in
// source/lua-cstack.c.
#define LUA_CSTACK_FULL_MARGIN      2304
#define LUA_CSTACK_OVERFULL_MARGIN  1792
// what handlers run at a safe point have before the first limit
#define LUA_SAFE_POINT_MARGIN       1536

// Past this many bytes in use, a call is "C stack overflow"
static inline uint32_t lua_cstack_full(uint32_t region) {
  return region - LUA_CSTACK_FULL_MARGIN;
}

// Past this many, even that error could overflow: it is thrown with none
static inline uint32_t lua_cstack_overfull(uint32_t region) {
  return region - LUA_CSTACK_OVERFULL_MARGIN;
}

// Past this many, a sleep runs no handlers (source/lua-events.c)
static inline uint32_t lua_safe_point_stack(uint32_t region) {
  return lua_cstack_full(region) - LUA_SAFE_POINT_MARGIN;
}

// How full the C stack is (luai_cstack): 0 when there is room, 1 past the
// first limit, 2 past the second
static inline int lua_cstack_level(uint32_t used, uint32_t region) {
  if (used > lua_cstack_overfull(region))
    return 2;
  if (used > lua_cstack_full(region))
    return 1;
  return 0;
}

#ifdef __cplusplus
}
#endif

#endif
