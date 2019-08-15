#ifndef __NET_PROTO_H__
#define __NET_PROTO_H__

#include <uuid/uuid.h>

#include "ltg_utils.h"

#if USE_EPOLL
#include <sys/epoll.h>
#endif

#define LNET_PORT_RANDOM 0

typedef int (*net_pack_handler)(const nid_t *nid, const sockid_t *sockid, ltgbuf_t *buf);
typedef int (*net_pack_len)(void *, uint32_t, int *msg_len, int *io_len);
typedef int (*net_event_write_handler)(struct epoll_event *ev, void *);
typedef int (*net_event_read_handler)(void *sock, void *ctx);

#define net_request_handler func_t

typedef enum {
        MSG_KEEP = LTG_MSG_MAX, //XXX:fix this type
        MSG_NET,
} net_progtype_t;

typedef struct {
        net_request_handler handler;
        void *context;
} net_prog_t;

typedef struct {
        uint32_t head_len; //length of proto head, not suitable for http
        net_pack_len pack_len;          /*return the length of a pack */
        net_event_read_handler reader;
        net_event_write_handler writer;
        net_pack_handler  pack_handler;
} net_proto_t;

#endif
