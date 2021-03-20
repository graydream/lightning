#ifndef __RPC_PROTO_H__
#define __RPC_PROTO_H__

#include "ltg_utils.h"
#include "ltg_net.h"

typedef int (*request_handler_func)(ltgbuf_t *input, ltgbuf_t *output,
                                    int *outlen, uint64_t *status);

typedef void (*request_get_handler)(const ltgbuf_t *buf, request_handler_func *func,
                                    const char **name);

typedef struct {
        coreid_t dist;
        sockid_t sockid;
        msgid_t msgid;
        ltgbuf_t buf;
        int replen;
        void *ctx;
        request_get_handler handler;
} rpc_request_t;

int rpc_pack_handler(const nid_t *nid, const sockid_t *sockid, ltgbuf_t *buf);
int rpc_pack_len(void *buf, uint32_t len, int *msg_len, int *io_len);

#define __RPC_HANDLER_NAME__ 128

inline static void S_LTG request_trans(void *arg, coreid_t *dist,
                                         sockid_t *sockid, msgid_t *msgid,
                                         ltgbuf_t *buf, int *replen, void **ctx)
{
        rpc_request_t *rpc_request;

        rpc_request = arg;
        ltgbuf_init(buf, 0);
        ltgbuf_merge(buf, &rpc_request->buf);
        *msgid = rpc_request->msgid;
        *sockid = rpc_request->sockid;

        if (dist)
                *dist = rpc_request->dist;

        if (replen)
                *replen = rpc_request->replen;
        
        if (ctx)
                *ctx = rpc_request->ctx;

        slab_stream_free(rpc_request);
}

typedef struct {
        request_get_handler handler;
        void *context;
} rpc_prog_t;

#endif
