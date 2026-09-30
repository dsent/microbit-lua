// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef RADIO_INBOX_H
#define RADIO_INBOX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The most a piece of a radio link carries, in bytes
#define RADIO_INBOX_BODY 26

// Makes the inbox, the first time a link opens, and empties it
void radio_inbox_open(void);

// Whether a piece would find no room: always, before any link opened
bool radio_inbox_full(void);

void radio_inbox_put(const uint8_t *body, int len);

// The oldest piece kept, if any
bool radio_inbox_take(uint8_t *body, int *len);

#ifdef __cplusplus
}
#endif

#endif
