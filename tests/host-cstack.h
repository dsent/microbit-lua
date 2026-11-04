// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef HOST_CSTACK_H
#define HOST_CSTACK_H

#include <stddef.h>

// Past this many bytes in use, Lua's C stack is full; (size_t)-1, never
extern size_t host_cstack_limit;

// The stack in use the first time a check found it past the limit, since a
// test set this to 0
extern size_t host_cstack_first_over;

void host_cstack_start(void);
size_t host_cstack_used(void);

#endif
