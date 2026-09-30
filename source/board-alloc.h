// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef BOARD_ALLOC_H
#define BOARD_ALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Memory from the board's heap, outside Lua's, for what the firmware makes
// only when a program first needs it; NULL when there is none. Supplied by
// the firmware, or by the host test, which counts it.
void *board_alloc(size_t size);
void board_free(void *p);

#ifdef __cplusplus
}
#endif

#endif
