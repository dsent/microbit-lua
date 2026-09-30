// -*- mode: c; indent-tabs-mode: nil; -*-
//
// A link over radio datagrams.
//
// Datagrams are 32 bytes and anyone on the group hears them all, so a frame
// says what it is and which link it belongs to. The link's number is one
// byte, drawn at random by the side that calls; a board takes the frames
// that carry its link's number. HELLO and WELCOME carry both ends' names in
// a fixed ten bytes, then the version of this protocol: a board with
// another version does not answer, and one that calls it is not answered.
//
// A message goes piece by piece, each sent again until the far end answers
// it. Its first piece is marked, so the far end knows where a message
// starts even when the one before it lost its end. The far end answers from
// the fiber that carries the radio's event, whatever its Lua is doing, and
// keeps the piece in its inbox for rx(): a line sent while it runs a command
// waits there, and runs after it. A piece that finds the inbox full is
// dropped unanswered, so the sender, once its tries are over, knows the
// piece was lost. A piece that comes again, because its answer was lost, is
// answered again and dropped. Pieces are numbered in 16 bits: a sender that
// fails comes back round to the number the far end last took only after
// 65,536 pieces, over four hours of tries.
//
// The inbox is made when the first link opens, so a program that never
// links pays nothing for it.

#include <string.h>

#include "board-alloc.h"
#include "radio-link.h"

// Kinds of frame. Other software on the same group starts its packets with
// small numbers too (MakeCode's packet types run from 0 up), so these sit
// where nothing else puts a first byte, and a stray packet is not read as
// one of ours.
#define HELLO   0xA1
#define WELCOME 0xA2
#define DATA    0xA3    // a piece that goes on with a message
#define ACK     0xA4
#define FIRST   0xA5    // a piece that starts one

// Version 1 marked no first pieces; version 2 numbered pieces in a byte;
// version 3 numbered links in 4 bytes
#define VERSION 4

// What a HELLO or a WELCOME carries: two names and the version
#define CALL (RADIO_NAME * 2 + 1)

// A piece is sent this many times, this many ms apart, before it is lost
#define TRIES 8
#define WAIT  30

struct RadioInbox {
  uint8_t first, count;
  uint8_t len[RADIO_INBOX_PIECES];
  bool starts[RADIO_INBOX_PIECES];
  uint8_t body[RADIO_INBOX_PIECES][RADIO_BODY];
};

static bool inbox_full(RadioLink *r) {
  return r->inbox->count == RADIO_INBOX_PIECES;
}

static void inbox_put(RadioLink *r, const uint8_t *body, int len,
                      bool starts) {
  RadioInbox *b = r->inbox;
  int at = (b->first + b->count) % RADIO_INBOX_PIECES;
  memcpy(b->body[at], body, len);
  b->len[at] = (uint8_t)len;
  b->starts[at] = starts;
  b->count++;
}

static bool inbox_take(RadioLink *r, uint8_t *body, int *len, bool *starts) {
  RadioInbox *b = r->inbox;
  if (b == NULL || b->count == 0)
    return false;
  *len = b->len[b->first];
  *starts = b->starts[b->first];
  memcpy(body, b->body[b->first], *len);
  b->first = (b->first + 1) % RADIO_INBOX_PIECES;
  b->count--;
  return true;
}


// kind, link, number, then body
static void put(RadioLink *r, uint8_t kind, uint8_t link, uint16_t num,
                const void *body, int len) {
  uint8_t f[RADIO_FRAME];
  if (len > RADIO_BODY) len = RADIO_BODY;
  f[0] = kind;
  f[1] = link;
  f[2] = (uint8_t)num;
  f[3] = (uint8_t)(num >> 8);
  if (len > 0) memcpy(f + RADIO_HEAD, body, len);
  r->air->send(r, f, len + RADIO_HEAD);
}

// Is this frame the one being waited for, and if so, what does it carry?
// A piece is wanted whether it starts a message or not, and says which in
// kind. Anything longer than a frame is not ours.
static bool wanted(const uint8_t *b, int n, uint8_t *kind, uint8_t link,
                   uint8_t *from, uint16_t *num, uint8_t *body, int *len) {
  if (n < RADIO_HEAD || n > RADIO_FRAME) return false;
  if (b[0] != *kind && !(*kind == DATA && b[0] == FIRST)) return false;
  if (link != 0 && b[1] != link) return false;
  *kind = b[0];
  if (from) *from = b[1];
  if (num) *num = (uint16_t)(b[2] | b[3] << 8);
  if (body) memcpy(body, b + RADIO_HEAD, n - RADIO_HEAD);
  if (len) *len = n - RADIO_HEAD;
  return true;
}

// Whether some wait of this board's may want the frame later: a call or
// its answer, or a piece or an answer on the open link. Another pair's
// traffic is dropped, and does not push out a frame that matters.
static bool worth_holding(RadioLink *r, const uint8_t *b, int n) {
  if (n < RADIO_HEAD || n > RADIO_FRAME) return false;
  switch (b[0]) {
  case HELLO: case WELCOME:
    return true;
  case DATA: case FIRST: case ACK:
    return r->link != 0 && b[1] == r->link;
  default:
    return false;
  }
}

// The next frame of this kind for this link: 1 when it came, 0 when one
// came that is not it, -1 when nothing did; kind is then the kind that
// came. Reading a datagram removes it, so one read while another is
// awaited, an ACK while the fiber of the radio's event looks for a piece,
// waits in the held slot for its reader.
static int take(RadioLink *r, uint8_t *kind, uint8_t link, uint8_t *from,
                uint16_t *num, uint8_t *body, int *len) {
  uint8_t f[RADIO_AIR_MAX];
  int n;
  if (r->held_len > 0
      && wanted(r->held, r->held_len, kind, link, from, num, body, len)) {
    r->held_len = 0;
    return 1;
  }
  n = r->air->recv(r, f);
  if (n < 0) return -1;
  if (wanted(f, n, kind, link, from, num, body, len)) return 1;
  if (worth_holding(r, f, n)) {
    memcpy(r->held, f, n);
    r->held_len = n;
  }
  return 0;
}

void radio_link_open(RadioLink *r, uint8_t link, const char *peer) {
  r->link = link;
  r->held_len = 0;
  r->out = 0;
  r->in = 0;
  memcpy(r->peer, peer, RADIO_NAME);
  r->peer[RADIO_NAME] = 0;
  if (r->inbox == NULL)
    r->inbox = (RadioInbox *)board_alloc(sizeof *r->inbox);
  if (r->inbox != NULL)
    r->inbox->first = r->inbox->count = 0;
}

// The answer to a HELLO, if this is it. A WELCOME names both ends, the one
// it goes to and the one it comes from, so a frame that only happens to
// share a kind and a link number with it, from some other board on the
// air, is not taken for one. hello is the call: the board called, then
// this one, then the version.
static bool welcomed(RadioLink *r, uint8_t link, const char *hello) {
  uint8_t kind = WELCOME, body[RADIO_BODY];
  int len;
  if (take(r, &kind, link, NULL, NULL, body, &len) != 1)
    return false;
  return len == CALL
    && memcmp(body, hello + RADIO_NAME, RADIO_NAME) == 0
    && memcmp(body + RADIO_NAME, hello, RADIO_NAME) == 0
    && body[RADIO_NAME * 2] == VERSION;
}

// Call once, then wait a moment for the answer
static bool called_once(RadioLink *r, uint8_t link, const char *hello) {
  uint32_t start;
  put(r, HELLO, link, 0, hello, CALL);
  start = r->air->now(r);
  while (r->air->now(r) - start < WAIT) {
    if (welcomed(r, link, hello)) return true;
    r->air->pause(r, 1);
  }
  return false;
}

bool radio_link_call(RadioLink *r, const char *them, const char *us,
                     uint8_t link, uint32_t timeout_ms) {
  char hello[CALL];
  uint32_t start = r->air->now(r);
  memcpy(hello, them, RADIO_NAME);
  memcpy(hello + RADIO_NAME, us, RADIO_NAME);
  hello[RADIO_NAME * 2] = VERSION;
  while (r->air->now(r) - start < timeout_ms) {
    if (called_once(r, link, hello)) {
      radio_link_open(r, link, them);
      r->serving = false;
      return true;
    }
  }
  return false;
}

// A HELLO addressed to this board, from the board it waits for, or from
// any if it waits for none, in this version: answer it and take the link
// it names. A call from anyone else is left unanswered, so the caller does
// not think it got through. Any other link the call replaces is dropped:
// the other end has started over, drawing a new number, which is how a
// reset board finds its way back. A call for the link already open is the
// caller calling again before the answer reached it: answered again, and
// what the link holds stays.
bool radio_link_called(RadioLink *r, const char *us, const char *from) {
  uint8_t kind = HELLO, body[RADIO_BODY];
  char welcome[CALL];
  uint8_t link;
  bool again;
  int len;
  if (take(r, &kind, 0, &link, NULL, body, &len) != 1
      || len != CALL
      || body[RADIO_NAME * 2] != VERSION
      || memcmp(body, us, RADIO_NAME) != 0) return false;
  if (from && memcmp(body + RADIO_NAME, from, RADIO_NAME) != 0)
    return false;
  again = r->serving && link == r->link
    && memcmp(body + RADIO_NAME, r->peer, RADIO_NAME) == 0;
  if (!again) {
    radio_link_open(r, link, (const char *)body + RADIO_NAME);
    r->serving = true;
  }
  memcpy(welcome, body + RADIO_NAME, RADIO_NAME);
  memcpy(welcome + RADIO_NAME, us, RADIO_NAME);
  welcome[RADIO_NAME * 2] = VERSION;
  put(r, WELCOME, link, 0, welcome, CALL);
  return !again;
}

// A piece the far end sent, if one came: answered and kept when there is
// room, answered again and dropped when it came before, and dropped
// unanswered when the inbox is full. With no inbox, which the board's heap
// could not give, a piece is left for rx.
static void keep(RadioLink *r) {
  uint8_t kind = DATA, body[RADIO_BODY];
  uint16_t num;
  int len;
  if (r->inbox == NULL
      || take(r, &kind, r->link, NULL, &num, body, &len) != 1)
    return;
  if (num != r->in) {
    if (inbox_full(r)) return;
    r->in = num;
    inbox_put(r, body, len, kind == FIRST);
  }
  put(r, ACK, r->link, num, NULL, 0);
}

// One piece, repeated until the far end answers it
static bool one(RadioLink *r, uint8_t kind, const char *body, int len) {
  r->out++;
  for (int n = 0; n < TRIES; n++) {
    uint32_t start;
    put(r, kind, r->link, r->out, body, len);
    start = r->air->now(r);
    while (r->air->now(r) - start < WAIT) {
      uint8_t got = ACK;
      uint16_t num;
      if (take(r, &got, r->link, NULL, &num, NULL, NULL) == 1
          && num == r->out) return true;
      keep(r);
      r->air->pause(r, 1);
    }
  }
  return false;
}

bool radio_link_tx(RadioLink *r, const char *msg, size_t len) {
  size_t sent = 0;
  if (r->link == 0) return false;
  do {
    int piece = len - sent > RADIO_BODY ? RADIO_BODY : (int)(len - sent);
    if (!one(r, sent == 0 ? FIRST : DATA, msg + sent, piece)) return false;
    sent += piece;
  } while (sent < len);
  return true;
}

// A frame that is not a new piece is passed over for the next one waiting,
// so a piece behind it is not left for a later call
bool radio_link_rx(RadioLink *r, uint8_t body[RADIO_BODY], int *len,
                   bool *starts) {
  if (inbox_take(r, body, len, starts)) return true;
  while (r->link != 0) {
    uint8_t kind = DATA;
    uint16_t num;
    int got = take(r, &kind, r->link, NULL, &num, body, len);
    if (got < 0) break;
    if (got == 1) {
      put(r, ACK, r->link, num, NULL, 0);
      if (num != r->in) {
        r->in = num;
        *starts = kind == FIRST;
        return true;
      }
    }
  }
  return false;
}

bool radio_link_heard(RadioLink *r) {
  if (r->link != 0)
    keep(r);
  return !r->serving;
}
