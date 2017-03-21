#pragma once

/*
 * Peers
 */

#include <c-macro.h>
#include <stdlib.h>

typedef struct Peer Peer;
typedef struct UserEntry UserEntry;

struct Peer {
        UserEntry *user;
};

int peer_new(Peer **peerp, UserEntry *user);
Peer *peer_free(Peer *peer);

C_DEFINE_CLEANUP(Peer *, peer_free);