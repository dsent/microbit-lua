// -*- mode: c; indent-tabs-mode: nil; -*-
#ifndef RADIO_LINK_H
#define RADIO_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A link over radio datagrams, in C only: the Lua bindings in
// source/codal-lua.cpp call it, and so does the fiber that carries the
// radio's event, which answers the far end while Lua is busy.

// A name is the five letters of a micro:bit's friendly name
#define RADIO_NAME 5

// The radio is documented to carry 32 bytes and the driver reports as many
// sent, but past 29 the tail arrives zeroed: measured between two boards,
// 28 and 29 come through whole, 30 loses its last byte and 31 and 32 lose
// two. So a frame is 29, and the driver's own limit is never reached. Its
// head is the kind of frame, the link's number (4 bytes) and the piece's
// number (2 bytes).
#define RADIO_HEAD  7
#define RADIO_FRAME 29
#define RADIO_BODY  (RADIO_FRAME - RADIO_HEAD)

// What the radio is asked for at once: a datagram longer than a frame
// reads as longer, and so is not taken for one
#define RADIO_AIR_MAX 32

// How many pieces the far end may send while Lua is busy
#define RADIO_INBOX_PIECES 8

typedef struct RadioLink RadioLink;
typedef struct RadioInbox RadioInbox;

// What a link needs of its board: its radio and its clock. recv gives the
// length of the next datagram, read into frame, or -1 with none; pause
// lets other fibers run.
typedef struct {
  void (*send)(RadioLink *r, const uint8_t *frame, int len);
  int (*recv)(RadioLink *r, uint8_t frame[RADIO_AIR_MAX]);
  uint32_t (*now)(RadioLink *r);
  void (*pause)(RadioLink *r, uint32_t ms);
} RadioAir;

struct RadioLink {
  const RadioAir *air;
  uint32_t link;                // 0 while no link is open
  uint16_t out;                 // number of the last piece sent
  uint16_t in;                  // number of the last piece taken
  bool serving;                 // the other board called this one
  char peer[RADIO_NAME + 1];
  // A frame taken off the air that the reader did not want, kept until
  // somebody wants it or another takes its place
  uint8_t held[RADIO_FRAME];
  int held_len;
  RadioInbox *inbox;            // made when the first link opens
};

// Both ends start a link the same way: numbering from nothing, the inbox
// made if it is not yet, and emptied
void radio_link_open(RadioLink *r, uint32_t link, const char *peer);

// Calls them, as us, on the given link number until they answer or the
// time is up. The number is the caller's to draw, at random and not 0: it
// is all that tells this pair's frames from another's on the same group.
bool radio_link_call(RadioLink *r, const char *them, const char *us,
                     uint32_t link, uint32_t timeout_ms);

// Whether a board called this one, us, and was answered: one look. With
// from, only that board is answered. A call repeated for the link already
// open, its answer having come late, is answered again and is no new
// caller: the link goes on as it was.
bool radio_link_called(RadioLink *r, const char *us, const char *from);

// The message, piece by piece, each answered before the next leaves;
// false when one is not, or no link is open
bool radio_link_tx(RadioLink *r, const char *msg, size_t len);

// The next piece the far end sent, if any, answered, and whether it
// starts a message: what came before it of a message whose end was lost
// never comes
bool radio_link_rx(RadioLink *r, uint8_t body[RADIO_BODY], int *len,
                   bool *starts);

// A datagram has come: while a link is open, a piece of it is answered
// and kept for rx, if there is room. Runs no Lua. Whether the radio's event
// goes on to Lua: not while this board serves a link, whose listen() takes
// what comes without it, so the link's traffic does not fill the line of
// events waiting for Lua.
bool radio_link_heard(RadioLink *r);

#ifdef __cplusplus
}
#endif

#endif
