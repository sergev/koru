// SPDX-License-Identifier: MIT
//
// The daemon's protocol server: frames in, replies out, and **no I/O at all**.
// The event loop does recv, feed, drain and nothing else, which is what lets
// the whole rejection matrix be driven from a test with no socket.
//
// Two rules hold here, one borrowed from koru and one that could not be.
//
//   C1, transposed: every accepted request frame produces exactly one reply.
//   Without it a client's `seq` map leaks a coroutine that never resumes. The
//   one exception is a parked KEY_READ, whose reply is owed and not yet
//   written, so the invariant is `replies + parked == frames`.
//
//   E1 splits. A frame that parses and is then rejected on *content* gets an
//   exact errno and the connection carries on. A frame that does not parse has
//   desynchronised the stream with no way to find the next boundary, so the
//   daemon sends one -EPROTO and closes. An ioctl has a private snapshot; a
//   byte stream does not.
#pragma once

#include "screen.h"

#include <ks_abi.h>

#include <cstdint>
#include <vector>

struct Server;

// One client. Owns its claims, and releasing them is its destructor's — which
// is what makes a client that is killed indistinguishable from one that asked
// to leave.
struct Conn;

// What the tests assert after every case, and what the fuzzer's oracle is.
struct ConnStats {
    uint64_t frames  = 0; // frames answered, malformed ones included
    uint64_t replies = 0; // reply frames emitted
    uint64_t bad     = 0; // frames rejected on content, which still reply
};

Server *server_new(Term *t);
void server_free(Server *s);

// The terminal the server draws on, for the daemon's own renderer.
Term &server_term(Server &s);

Conn *conn_new(Server &s);

// Releases the claims, restoring the screen the way Braam's ~FullScreen does.
void conn_free(Conn *c);

// Bytes in, replies appended to this connection's out buffer. Pure: it reads
// and writes the grid and the connection, and touches nothing else.
//
// False when the connection must be closed — which happens only after a
// -EPROTO, and the reply for it is already in the out buffer.
bool feed(Conn &c, const uint8_t *data, size_t n);

// End of input. Nothing is owed a reply, and the claims go back.
void conn_eof(Conn &c);

// The replies waiting to be written, and how to say they have been.
const uint8_t *conn_out(const Conn &c, size_t &n);
void conn_drain(Conn &c, size_t n);

bool conn_closed(const Conn &c);
const ConnStats &conn_stats(const Conn &c);

// A key from the window. It answers a parked KEY_READ if there is one, and is
// dropped if nobody holds the keys — a keystroke with no reader is not queued
// for ever.
void server_key(Server &s, uint32_t code, uint32_t mods);

// The window changed shape. The grid is resized, and a parked KEY_READ is
// answered -EINTR *with a payload*: a resize has to be answered rather than
// signalled, because the daemon owns the reply and cannot invent a key.
void server_resize(Server &s, uint32_t cols, uint32_t rows);

// Whether this connection holds each claim, for the daemon and the tests.
bool conn_has_keys(const Conn &c);
bool conn_has_screen(const Conn &c);

// Whether a KEY_READ is parked here: the one frame whose reply is owed rather
// than written, and the term the C1 invariant needs to balance.
bool conn_parked(const Conn &c);
