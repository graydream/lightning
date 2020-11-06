#ifndef __RPC_PROTO_H__
#define __RPC_PROTO_H__

#include "ltg_utils.h"
#include "ltg_net.h"

typedef struct {
        coreid_t dist;
        sockid_t sockid;
        msgid_t msgid;
        ltgbuf_t buf;
        void *ctx;
} rpc_request_t;

int rpc_pack_handler(const nid_t *nid, const sockid_t *sockid, ltgbuf_t *buf);
int rpc_pack_len(void *buf, uint32_t len, int *msg_len, int *io_len);

typedef int (*__request_handler_func__)(const sock_t *sockid, const msgid_t *msgid,
                                        ltgbuf_t *input, ltgbuf_t *output);
#define __RPC_HANDLER_NAME__ 128

inline static void IO_FUNC request_trans(void *arg, coreid_t *dist, sockid_t *sockid, msgid_t *msgid, ltgbuf_t *buf, void **ctx)
{
        rpc_request_t *rpc_request;

        rpc_request = arg;
        ltgbuf_init(buf, 0);
        ltgbuf_merge(buf, &rpc_request->buf);
        *msgid = rpc_request->msgid;
        *sockid = rpc_request->sockid;

        if (dist)
                *dist = rpc_request->dist;

        if (ctx)
                *ctx = rpc_request->ctx;

        slab_stream_free(rpc_request);
}

#endif
