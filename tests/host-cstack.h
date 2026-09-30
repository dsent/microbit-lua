// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef HOST_CSTACK_H
#define HOST_CSTACK_H

#include <stddef.h>

// Past this many bytes in use, Lua's C stack is full; (size_t)-1, never
extern size_t host_cstack_limit;

void host_cstack_start(void);
size_t host_cstack_used(void);

#endif
