#ifndef __SOCKOP_H__
#define __SOCKOP_H__

#include "mem/ltgbuf.h"

typedef enum {
        NET_HANDLE_NULL,
        NET_HANDLE_PERSISTENT, /*constant connect*/
        NET_HANDLE_TRANSIENT, /*temporary connect*/
} net_handle_type_t;

typedef struct {
        void *rdma_handler;
        uint32_t addr;
        uint32_t seq;
        int sd;
        int type;

        int (*request)(void *ctx, void *args);
        void (*reply)(void *ctx, void *args);
} sock_t;

typedef sock_t sockid_t;

#define SOCKID_DUMP_L(LEVEL, sockid) do { \
        LEVEL("sock %s/%d seq %u type %d rdma %p\n", \
              _inet_ntoa((sockid)->addr), \
              (sockid)->sd, \
              (sockid)->seq, \
              (sockid)->type, \
              (sockid)->rdma_handler); \
} while (0)

#define SOCKID_DUMP(sockid) SOCKID_DUMP_L(DBUG, sockid)

typedef struct {
        net_handle_type_t type;
        union {
                nid_t nid;
                sockid_t  sd;
        } u;
} net_handle_t;

static inline int sockid_cmp(const sockid_t *sock1, const sockid_t *sock2)
{
        if (sock1->addr < sock2->addr)
                return -1;
        else if (sock1->addr > sock2->addr)
                return 1;

        if (sock1->seq < sock2->seq)
                return -1;
        else if (sock1->seq > sock2->seq)
                return 1;

        if (sock1->sd < sock2->sd)
                return -1;
        else if (sock1->sd > sock2->sd)
                return 1;

        return 0;
}

static inline void id2nh(net_handle_t *nh, const nid_t *id)
{
        nh->u.nid = *id;
        nh->type = NET_HANDLE_PERSISTENT;
}

static inline void sock2nh(net_handle_t *nh, const sockid_t *id)
{
        nh->u.sd = *id;
        nh->type = NET_HANDLE_TRANSIENT;
}

typedef struct {
        const sockid_t *sockid;
        const msgid_t *msgid;
        int err;
        uint64_t latency;
        ltgbuf_t *buf;
} sockop_reply_t;

#endif
