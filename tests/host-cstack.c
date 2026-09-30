// -*- mode: c; indent-tabs-mode: nil; -*-
//
// luai_cstack for the host tests: the C stack in use is measured from the
// point host_cstack_start() was called, against a limit a test sets. The
// firmware's limits are for its 8 KB region; the host's frames are larger,
// and its stack far bigger, so a test chooses its own. Past the limit by
// more than a level of calls takes, 1 KB here as 512 bytes on the board,
// even raising the error is too much.

#include <stddef.h>
#include <stdint.h>

#include "host-cstack.h"

static uintptr_t base;
size_t host_cstack_limit = (size_t)-1;
size_t host_cstack_first_over;

void host_cstack_start(void) {
  base = (uintptr_t)__builtin_frame_address(0);
}

size_t host_cstack_used(void) {
  uintptr_t here = (uintptr_t)__builtin_frame_address(0);
  return base != 0 ? (size_t)(base - here) : 0;
}

int luai_cstack(void) {
  size_t used = host_cstack_used();
  if (host_cstack_limit == (size_t)-1 || used <= host_cstack_limit)
    return 0;
  if (host_cstack_first_over == 0)
    host_cstack_first_over = used;
  return used > host_cstack_limit + 1024 ? 2 : 1;
}
