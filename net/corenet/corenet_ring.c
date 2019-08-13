#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#include <semaphore.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <sys/mman.h>

#define DBG_SUBSYS S_LTG_NET

#include "macros.h"
#include "ltg_conf.h"
#include "ltg_core.h"
#include "corenet.h"
#include "corenet_maping.h"
#include "corerpc.h"
#include "dbg.h"
#include "tiny_mem.h"
#include "huge_mem.h"
#include "ltg_net.h"
#include "ltg_net.h"
#include "net_table.h"
#include "rpc_table.h"
#include "sche.h"
#include "ltg_misc.h"
#include "ltg_utils.h"
#include "timer.h"
#include "libringbuf.h"
#include "variable.h"
#include "ltg_conf.h"

typedef corenet_ring_ltgbuf_t corenet_node_t;

static void IO_FUNC *__corenet_get_byctx(void *ctx)
{
        return core_tls_getfrom1(ctx, VARIABLE_CORENET_RING);
}

static void IO_FUNC *__corenet_get()
{
        return core_tls_get(VARIABLE_CORENET_RING);
}

int corenet_ring_init(int max, corenet_ring_t **_corenet)
{
        int ret, len, i, size;
        corenet_ring_t *corenet;
        corenet_node_t *node;

        size = max;

        DINFO("ring malloc %llu\n", (LLU)sizeof(corenet_node_t) * size);
        len = sizeof(corenet_ring_t) + sizeof(corenet_node_t) * size;
        ret = ltg_malloc((void **)&corenet, sizeof(corenet_ring_t) + max * sizeof(corenet_node_t));
        if (ret)
                GOTO(err_ret, ret);

        memset(corenet, 0x0, len);

        corenet->size = size;

        for (i = 0; i < size; i++) {
                node = &corenet->array[i];

                INIT_LIST_HEAD(&node->hook);
                ltgbuf_init(&node->queue_buf, 0);
                node->send_buf = NULL;
                node->recv_buf = NULL;
                node->exec = NULL;
        }

        core_tls_set(VARIABLE_CORENET_RING, corenet);

        if (_corenet)
                *_corenet = corenet;

        DBUG("corenet init done\n");

        return 0;
err_ret:
        return ret;
}

void IO_FUNC corenet_ring_poll(corenet_ring_t *corenet)
{
        corenet_node_t *node;
        void *req;

        for (int i = 0; i < corenet->size; i++) {
                node = &corenet->array[i];
                if (node->recv_buf && !libringbuf_empty(node->recv_buf)) {
                        libringbuf_sc_dequeue(node->recv_buf, &req);
                        //ring_net->exec(req);
                        UNIMPLEMENTED(__DUMP__);
                }
        }
}

int IO_FUNC corenet_ring_connected(const sockid_t *sockid)
{
        corenet_ring_t *__corenet__ = __corenet_get();
        corenet_node_t *node = &__corenet__->array[sockid->sd];

        return node->send_buf ? 1 : 0;
}

static int __corenet_ring_connect__(va_list ap)
{
        corenet_ring_t *corenet = __corenet_get();
        corenet_node_t *node;

        void **ptr = va_arg(ap, void **);

        va_end(ap);

        for (int i = 0; i < corenet->size; i++) {
                node = &corenet->array[i];
                if (node->recv_buf) {
                        continue;
                }

                node->recv_buf = libringbuf_create((1<<12), RING_F_SP_ENQ|RING_F_SC_DEQ);

                *ptr =  node->recv_buf;
        }

        UNIMPLEMENTED(__DUMP__);
        
        return 0;
}

static void *__corenet_ring_connect(const coreid_t *coreid)
{
        int ret;
        void *ptr;

        ret = core_request(coreid->idx, -1, "ring_connect",
                           __corenet_ring_connect__, &ptr);
        if (ret)
                UNIMPLEMENTED(__DUMP__);

        return ptr;
}

int corenet_ring_connect(const coreid_t *coreid, sockid_t *sockid)
{
        int i;
        corenet_ring_t *corenet = __corenet_get();
        corenet_node_t *node;

        for (i = 0; i < corenet->size; i++) {
                node = &corenet->array[i];
                if (node->send_buf) {
                        continue;
                }

                sockid->sd = i;
                sockid->type = SOCKID_CORENET;
                node->send_buf = __corenet_ring_connect(coreid);

                break;
        }

        LTG_ASSERT(i < corenet->size);

        return 0;
}

void corenet_ring_close(const sockid_t *_sockid)
{
        (void) _sockid;

        UNIMPLEMENTED(__DUMP__);
}

int corenet_ring_send(void *ctx, const sockid_t *sockid, ltgbuf_t *buf)
{
        corenet_ring_t *__corenet__ = __corenet_get_byctx(ctx);
        corenet_node_t *node = &__corenet__->array[sockid->sd];

        LTG_ASSERT(sockid->type == SOCKID_CORENET);
        
        void *req = ltgbuf_head(buf);
        if (likely(!libringbuf_full(node->send_buf))) {
                libringbuf_sp_enqueue(node->send_buf, &req);
        } else {
                UNIMPLEMENTED(__DUMP__);
                //ltgbuf_merge(&buffer->queue_buf ,buf);
        }

        return 0;
}
