// -*- mode: c; indent-tabs-mode: nil; -*-
//
// Pieces the far end of a radio link sent while this end waited on its own
// to be answered: each answered at once, since the far end may be waiting
// in the same way, and kept here for rx(). Lua runs one call at a time, so
// the rx() that would have answered them waits until the tx() that is
// waiting is done. A piece that finds the inbox full is left unanswered,
// and the far end sends it again.
//
// The inbox is made when the first link opens, so a program that never
// links pays nothing for it.

#include <string.h>

#include "board-alloc.h"
#include "radio-inbox.h"

#define PIECES 2

typedef struct {
  int first, count;
  int len[PIECES];
  uint8_t body[PIECES][RADIO_INBOX_BODY];
} Inbox;

static Inbox *inbox;

void radio_inbox_open(void) {
  if (inbox == NULL)
    inbox = (Inbox *)board_alloc(sizeof *inbox);
  if (inbox != NULL)
    inbox->first = inbox->count = 0;
}

bool radio_inbox_full(void) {
  return inbox == NULL || inbox->count == PIECES;
}

void radio_inbox_put(const uint8_t *body, int len) {
  int at;
  if (radio_inbox_full())
    return;
  at = (inbox->first + inbox->count) % PIECES;
  memcpy(inbox->body[at], body, len);
  inbox->len[at] = len;
  inbox->count++;
}

bool radio_inbox_take(uint8_t *body, int *len) {
  if (inbox == NULL || inbox->count == 0)
    return false;
  memcpy(body, inbox->body[inbox->first], inbox->len[inbox->first]);
  *len = inbox->len[inbox->first];
  inbox->first = (inbox->first + 1) % PIECES;
  inbox->count--;
  return true;
}
