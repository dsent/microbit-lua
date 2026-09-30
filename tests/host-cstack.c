// -*- mode: c; indent-tabs-mode: nil; -*-
//
// luai_cstack for the host tests: the C stack in use is measured from the
// point host_cstack_start() was called, against a limit a test sets. The
// firmware's limits are for its 8 KB region; the host's frames are larger,
// and its stack far bigger, so a test chooses its own.

#include <stddef.h>
#include <stdint.h>

#include "host-cstack.h"

static uintptr_t base;
size_t host_cstack_limit = (size_t)-1;

void host_cstack_start(void) {
  base = (uintptr_t)__builtin_frame_address(0);
}

size_t host_cstack_used(void) {
  uintptr_t here = (uintptr_t)__builtin_frame_address(0);
  return base != 0 ? (size_t)(base - here) : 0;
}

int luai_cstack(void) {
  size_t used = host_cstack_used();
  if (host_cstack_limit == (size_t)-1)
    return 0;
  if (used > host_cstack_limit + 512)
    return 2;
  return used > host_cstack_limit;
}
