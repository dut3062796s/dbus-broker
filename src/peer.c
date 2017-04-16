/*
 * Peers
 */

#include <c-dvar.h>
#include <c-dvar-type.h>
#include <c-macro.h>
#include <c-rbtree.h>
#include <c-string.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "bus.h"
#include "dbus-protocol.h"
#include "driver.h"
#include "match.h"
#include "message.h"
#include "peer.h"
#include "reply.h"
#include "socket.h"
#include "user.h"
#include "util/dispatch.h"
#include "util/fdlist.h"

static int peer_forward_method_call(Peer *sender, const char *destination, uint32_t serial, Message *message) {
        _c_cleanup_(reply_slot_freep) ReplySlot *slot = NULL;
        Peer *receiver;
        int r;

        receiver = bus_find_peer_by_name(sender->bus, destination);
        if (!receiver)
                return -EBADMSG;

        if (!(message->header->flags & DBUS_HEADER_FLAG_NO_REPLY_EXPECTED)) {
                r = reply_slot_new(&slot, &receiver->replies_outgoing, sender, serial);
                if (r < 0)
                        return r;
        }

        r = socket_queue_message(receiver->socket, message);
        if (r)
                return (r > 0) ? -ENOTRECOVERABLE : r;

        dispatch_file_select(&receiver->dispatch_file, EPOLLOUT);

        slot = NULL;

        return 0;
}

static int peer_forward_reply(Peer *sender, const char *destination, uint32_t reply_serial, Message *message) {
        ReplySlot *slot;
        uint64_t id;
        int r;

        r = peer_id_from_unique_name(destination, &id);
        if (r < 0)
                return r;

        slot = reply_slot_get_by_id(&sender->replies_outgoing, id, reply_serial);
        if (!slot)
                return -EBADMSG;

        r = socket_queue_message(slot->sender->socket, message);
        if (r)
                return (r > 0) ? -ENOTRECOVERABLE : r;

        dispatch_file_select(&slot->sender->dispatch_file, EPOLLOUT);

        reply_slot_free(slot);

        return 0;
}

static int peer_forward_broadcast(Peer *sender, const char *interface, const char *member, const char *path, Message *message) {
        return 0;
}

static int peer_dispatch_read_message(Peer *peer) {
        static const CDVarType type[] = {
                C_DVAR_T_INIT(
                        C_DVAR_T_TUPLE7(
                                C_DVAR_T_y,
                                C_DVAR_T_y,
                                C_DVAR_T_y,
                                C_DVAR_T_y,
                                C_DVAR_T_u,
                                C_DVAR_T_u,
                                C_DVAR_T_ARRAY(
                                        C_DVAR_T_TUPLE2(
                                                C_DVAR_T_y,
                                                C_DVAR_T_v
                                        )
                                )
                        )
                ), /* (yyyyuua(yv)) */
        };
        _c_cleanup_(message_unrefp) Message *message = NULL;
        _c_cleanup_(c_dvar_freep) CDVar *v = NULL;
        const char *path = NULL,
                   *interface = NULL,
                   *member = NULL,
                   *error_name = NULL,
                   *destination = NULL,
                   *sender = NULL,
                   *signature = "";
        uint32_t serial = 0, reply_serial = 0, n_fds = 0;
        uint8_t field;
        int r;

        r = socket_read_message(peer->socket, &message);
        if (r < 0)
                return r;

        /*
         * XXX: Rather than allocating @v, we should use its static versions on the stack,
         *      once provided by c-dvar.
         */

        r = c_dvar_new(&v);
        if (r)
                return (r > 0) ? -ENOTRECOVERABLE : r;

        c_dvar_begin_read(v, message->big_endian, type, message->header, message->n_header);

        c_dvar_read(v, "(yyyyuu[", NULL, NULL, NULL, NULL, NULL, &serial);

        while (c_dvar_more(v)) {
                /*
                 * XXX: What should we do on duplicates?
                 */

                c_dvar_read(v, "(y", &field);

                switch (field) {
                case DBUS_MESSAGE_FIELD_INVALID:
                        return -EBADMSG;
                case DBUS_MESSAGE_FIELD_PATH:
                        c_dvar_read(v, "<o>)", c_dvar_type_o, &path);
                        break;
                case DBUS_MESSAGE_FIELD_INTERFACE:
                        c_dvar_read(v, "<s>)", c_dvar_type_s, &interface);
                        break;
                case DBUS_MESSAGE_FIELD_MEMBER:
                        c_dvar_read(v, "<s>)", c_dvar_type_s, &member);
                        break;
                case DBUS_MESSAGE_FIELD_ERROR_NAME:
                        c_dvar_read(v, "<s>)", c_dvar_type_s, &error_name);
                        break;
                case DBUS_MESSAGE_FIELD_REPLY_SERIAL:
                        c_dvar_read(v, "<u>)", c_dvar_type_u, &reply_serial);
                        break;
                case DBUS_MESSAGE_FIELD_DESTINATION:
                        c_dvar_read(v, "<s>)", c_dvar_type_s, &destination);
                        break;
                case DBUS_MESSAGE_FIELD_SENDER:
                        /* XXX: check with dbus-daemon(1) on what to do */
                        c_dvar_read(v, "<s>)", c_dvar_type_s, &sender);
                        break;
                case DBUS_MESSAGE_FIELD_SIGNATURE:
                        c_dvar_read(v, "<g>)", c_dvar_type_g, &signature);
                        break;
                case DBUS_MESSAGE_FIELD_UNIX_FDS:
                        c_dvar_read(v, "<u>)", c_dvar_type_u, &n_fds);
                        break;
                default:
                        c_dvar_skip(v, "v)");
                        break;
                }
        }

        c_dvar_read(v, "])");

        r = c_dvar_end_read(v);
        if (r)
                return (r > 0) ? -EBADMSG : r;

        if (message->fds) {
                if (_c_unlikely_(n_fds > fdlist_count(message->fds)))
                        return -EBADMSG;

                fdlist_truncate(message->fds, n_fds);
        }

        if (_c_unlikely_(c_string_equal(destination, "org.freedesktop.DBus")))
                return driver_dispatch_interface(peer, serial, interface, member, path, signature, message);

        /* XXX: verify message contents, append sender */

        if (message->header->type != DBUS_MESSAGE_TYPE_METHOD_CALL)
                message->header->flags |= DBUS_HEADER_FLAG_NO_REPLY_EXPECTED;

        if (!destination) {
                if (message->header->type != DBUS_MESSAGE_TYPE_SIGNAL)
                        return -EBADMSG;

                return peer_forward_broadcast(peer, interface, member, path, message);
        }

        switch (message->header->type) {
        case DBUS_MESSAGE_TYPE_SIGNAL:
        case DBUS_MESSAGE_TYPE_METHOD_CALL:
                return peer_forward_method_call(peer, destination, serial, message);
        case DBUS_MESSAGE_TYPE_METHOD_REPLY:
        case DBUS_MESSAGE_TYPE_ERROR:
                return peer_forward_reply(peer, destination, reply_serial, message);
        }

        return 0;
}

static int peer_dispatch_read_line(Peer *peer) {
        char *line_in, *line_out;
        size_t *pos, n_line;
        int r;

        r = socket_read_line(peer->socket, &line_in, &n_line);
        if (r < 0)
                return r;

        r = socket_queue_line(peer->socket, SASL_MAX_OUT_LINE_LENGTH, &line_out, &pos);
        if (r < 0)
                return r;

        r = sasl_dispatch(&peer->sasl, line_in, line_out, pos);
        if (r < 0)
                return r;
        else if (r == 0)
                dispatch_file_select(&peer->dispatch_file, EPOLLOUT);
        else
                peer->authenticated = true;

        return 0;
}

static int peer_dispatch_read(Peer *peer) {
        int r;

        for (unsigned int i = 0; i < 32; i ++) {
                if (_c_likely_(peer->authenticated)) {
                        r = peer_dispatch_read_message(peer);
                } else {
                        r = peer_dispatch_read_line(peer);
                }
                if (r == -EAGAIN) {
                        /* nothing to be done */
                        dispatch_file_clear(&peer->dispatch_file, EPOLLIN);
                        return 0;
                } else if (r < 0) {
                        /* XXX: swallow error code and tear down this peer */
                        return 0;
                }
        }

        return 0;
}

static int peer_dispatch_write(Peer *peer) {
        int r;

        r = socket_write(peer->socket);
        if (r == -EAGAIN) {
                /* not able to write more */
                dispatch_file_clear(&peer->dispatch_file, EPOLLOUT);
                return 0;
        } else if (r == 0) {
                /* nothing more to write */
                dispatch_file_deselect(&peer->dispatch_file, EPOLLOUT);
        } else if (r < 0) {
                /* XXX: swallow error code and tear down this peer */
                return 0;
        }

        return 0;
}

int peer_dispatch(DispatchFile *file, uint32_t mask) {
        Peer *peer = c_container_of(file, Peer, dispatch_file);
        int r;

        if (mask & EPOLLIN) {
                r = peer_dispatch_read(peer);
                if (r < 0)
                        return r;
        }

        if (mask & EPOLLOUT) {
                r = peer_dispatch_write(peer);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int peer_get_peersec(int fd, char **labelp, size_t *lenp) {
        _c_cleanup_(c_freep) char *label = NULL;
        char *l;
        socklen_t len = 1023;
        int r;

        label = malloc(len + 1);
        if (!label)
                return -ENOMEM;

        for (;;) {
                r = getsockopt(fd, SOL_SOCKET, SO_PEERSEC, label, &len);
                if (r >= 0) {
                        label[len] = '\0';
                        *lenp = len;
                        *labelp = label;
                        label = NULL;
                        break;
                } else if (errno == ENOPROTOOPT) {
                        *lenp = 0;
                        *labelp = NULL;
                        break;
                } else if (errno != ERANGE)
                        return -errno;

                l = realloc(label, len + 1);
                if (!l)
                        return -ENOMEM;

                label = l;
        }

        return 0;
}

static int peer_compare(CRBTree *tree, void *k, CRBNode *rb) {
        Peer *peer = c_container_of(rb, Peer, rb);
        uint64_t id = *(uint64_t*)k;

        if (peer->id < id)
                return -1;
        if (peer->id > id)
                return 1;

        return 0;
}

/**
 * peer_new() - XXX
 */
int peer_new(Peer **peerp,
             Bus *bus,
             int fd) {
        _c_cleanup_(peer_freep) Peer *peer = NULL;
        _c_cleanup_(user_entry_unrefp) UserEntry *user = NULL;
        _c_cleanup_(c_freep) char *seclabel = NULL;
        CRBNode **slot, *parent;
        size_t n_seclabel;
        struct ucred ucred;
        socklen_t socklen = sizeof(ucred);
        int r;

        r = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &socklen);
        if (r < 0)
                return -errno;

        r = user_registry_ref_entry(&bus->users, &user, ucred.uid);
        if (r < 0)
                return r;

        if (user->n_peers < 1)
                return -EDQUOT;

        r = peer_get_peersec(fd, &seclabel, &n_seclabel);
        if (r < 0)
                return r;

        peer = calloc(1, sizeof(*peer));
        if (!peer)
                return -ENOMEM;

        user->n_peers --;

        peer->bus = bus;
        c_rbnode_init(&peer->rb);
        peer->user = user;
        user = NULL;
        peer->pid = ucred.pid;
        peer->seclabel = seclabel;
        seclabel = NULL;
        peer->n_seclabel = n_seclabel;
        peer->dispatch_file = (DispatchFile)DISPATCH_FILE_NULL(peer->dispatch_file);
        sasl_init(&peer->sasl, ucred.uid, bus->guid);
        match_registry_init(&peer->matches);
        reply_registry_init(&peer->replies_outgoing);
        peer->replies_incoming = (CList)C_LIST_INIT(peer->replies_incoming);

        r = dispatch_file_init(&peer->dispatch_file,
                               &bus->dispatcher,
                               &bus->ready_list,
                               peer_dispatch,
                               fd,
                               EPOLLIN | EPOLLOUT);
        if (r < 0)
                return r;

        r = socket_new(&peer->socket, fd);
        if (r < 0)
                return r;

        peer->id = bus->peers.ids++;
        slot = c_rbtree_find_slot(&bus->peers.peers, peer_compare, &peer->id, &parent);
        assert(slot); /* peer->id is guaranteed to be unique */
        c_rbtree_add(&bus->peers.peers, parent, slot, &peer->rb);

        *peerp = peer;
        peer = NULL;
        return 0;
}

/**
 * peer_free() - XXX
 */
Peer *peer_free(Peer *peer) {
        CRBNode *node, *next;
        ReplySlot *reply, *safe;

        if (!peer)
                return NULL;

        assert(!peer->names.root);
        assert(!peer->registered);

        peer->user->n_peers ++;

        for (node = c_rbtree_first_postorder(&peer->match_rules), next = c_rbnode_next_postorder(node);
             node;
             node = next, next = c_rbnode_next_postorder(node)) {
                MatchRule *rule = c_container_of(node, MatchRule, rb_peer);

                match_rule_free(rule);
        }

        c_list_for_each_entry_safe(reply, safe, &peer->replies_incoming, link)
                reply_slot_free(reply);

        c_rbtree_remove_init(&peer->bus->peers.peers, &peer->rb);

        dispatch_file_deinit(&peer->dispatch_file);
        reply_registry_deinit(&peer->replies_outgoing);
        match_registry_deinit(&peer->matches);
        sasl_deinit(&peer->sasl);
        socket_free(peer->socket);
        user_entry_unref(peer->user);
        free(peer->seclabel);
        free(peer);

        return NULL;
}

void peer_start(Peer *peer) {
        return dispatch_file_select(&peer->dispatch_file, EPOLLIN);
}

void peer_stop(Peer *peer) {
        return dispatch_file_deselect(&peer->dispatch_file, EPOLLIN);
}

void peer_register(Peer *peer) {
        assert(!peer->registered);

        peer->registered = true;

        driver_notify_name_owner_change(NULL, NULL, peer);
}

void peer_unregister(Peer *peer) {
        assert(peer->registered);

        driver_notify_name_owner_change(NULL, peer, NULL);

        peer->registered = false;
}

void peer_registry_init(PeerRegistry *registry) {
        registry->peers = (CRBTree){};
        registry->ids = 0;
}

void peer_registry_deinit(PeerRegistry *registry) {
        assert(!registry->peers.root);
}

void peer_registry_flush(PeerRegistry *registry) {
        CRBNode *node1, *next1;

        for (node1 = c_rbtree_first_postorder(&registry->peers), next1 = c_rbnode_next_postorder(node1);
             node1;
             node1 = next1, next1 = c_rbnode_next(node1)) {
                Peer *peer = c_container_of(node1, Peer, rb);
                CRBNode *node2, *next2;

                for (node2 = c_rbtree_first_postorder(&peer->names), next2 = c_rbnode_next_postorder(node2);
                     node2;
                     node2 = next2, next2 = c_rbnode_next_postorder(node2)) {
                        NameOwner *owner = c_container_of(node2, NameOwner, rb);

                        name_owner_free(owner);
                }

                for (node2 = c_rbtree_first_postorder(&peer->replies_outgoing.slots), next2 = c_rbnode_next_postorder(node2);
                     node2;
                     node2 = next2, next2 = c_rbnode_next_postorder(node2)) {
                        ReplySlot *slot = c_container_of(node2, ReplySlot, rb);

                        reply_slot_free(slot);
                }

                peer->registered = false;

                peer_free(peer);
        }
}

Peer *peer_registry_find_peer(PeerRegistry *registry, uint64_t id) {
        Peer *peer;

        peer = c_rbtree_find_entry(&registry->peers, peer_compare, &id, Peer, rb);

        return peer->registered ? peer : NULL;
}

int peer_id_from_unique_name(const char *name, uint64_t *idp) {
        uint64_t id;
        char *end;

        if (strlen(name) < strlen(":1."))
                return -EINVAL;

        name += strlen(":1.");

        errno = 0;
        id = strtoull(name, &end, 10);
        if (errno != 0)
                return -errno;
        if (*end || name == end)
                return -EINVAL;

        *idp = id;
        return 0;
}
