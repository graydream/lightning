#include <limits.h>
#include <time.h>
#include <string.h>
#include <sys/epoll.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_net.h"
#include "ltg_utils.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

#define CORENET_MAGIC 0x347a8447

typedef struct {
        uint32_t magic;
        coreid_t from;
        coreid_t to;
} corenet_msg_t;

typedef struct {
        coreid_t coreid;
        int sd;
} __corenet_tcp_t;

extern int ltg_nofile_max;

/**
 * 包括两步骤：
 * - 建立连接: nid
 * - 协商core hash
 *
 * @param nid
 * @param sockid
 * @return
 */
int corenet_tcp_connect(const coreid_t *coreid, uint32_t addr, uint32_t port,
                        sockid_t *sockid)
{
        int ret;
        net_handle_t nh;
        corenet_msg_t msg;
        corerpc_ctx_t *ctx;
        struct sockaddr_in sin;

        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;

        sin.sin_addr.s_addr = addr;
        sin.sin_port = port;

        DBUG("connect %s:%u\n", inet_ntoa(sin.sin_addr), ntohs(port));

        ret = core_getid(&msg.from);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = tcp_sock_connect(&nh, &sin, 0, 3, 0);
        if (unlikely(ret)) {
                DINFO("try to connect %s:%u (%u) %s\n", inet_ntoa(sin.sin_addr),
                      ntohs(port), ret, strerror(ret));
                GOTO(err_ret, ret);
        }

        msg.to = *coreid;
        msg.magic = CORENET_MAGIC;

        ret = send(nh.u.sd.sd, &msg, sizeof(msg), 0);
        if (ret < 0) {
                ret = errno;
                UNIMPLEMENTED(__DUMP__);
        }

        sockid->sd = nh.u.sd.sd;
        sockid->addr = nh.u.sd.addr;
        sockid->seq = _random();
        sockid->type = SOCKID_CORENET;

        ret = slab_static_alloc1((void **)&ctx, sizeof(*ctx));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = tcp_sock_tuning(sockid->sd, 1, 1);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        ctx->running = 0;
        ctx->sockid = *sockid;
        ctx->coreid = *coreid;
        ret = corenet_tcp_add(NULL, sockid, ctx, corerpc_recv, corerpc_close,
                              NULL, NULL, netable_rname(&coreid->nid));
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        LTG_ASSERT(sockid->sd < ltg_nofile_max);

        return 0;
err_ret:
        return ret;
}

STATIC void *__corenet_tcp_accept__(void *arg)
{
        int ret;
        char buf[MAX_BUF_LEN];
        corenet_msg_t *msg;
        sockid_t *sockid;
        core_t *core;
        corerpc_ctx_t *ctx = arg;

        sockid = &ctx->sockid;

        DBUG("accept from %s, sd %d\n",  _inet_ntoa(sockid->addr), sockid->sd);

        ret = sock_poll_sd(sockid->sd, 1000 * 1000, POLLIN);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = recv(sockid->sd, buf, sizeof(*msg), 0);
        if (ret < 0) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        if (ret == 0) {
                DWARN("peer closed\n");
                ret = ECONNRESET;
                GOTO(err_ret, ret);
        }

        msg = (void*)buf;

        if (msg->magic != CORENET_MAGIC) {
                ret = EINVAL;
                DERROR("got bad magic %x\n", msg->magic);
                GOTO(err_ret, ret);
        }

        LTG_ASSERT(sizeof(*msg) == ret);
        LTG_ASSERT(coreid_cmp(&msg->to, &ctx->local) == 0);

        ret = tcp_sock_tuning(sockid->sd, 1, 1);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        core = core_get(msg->to.idx);
        ctx->coreid = msg->from;

        DBUG("core[%d] %p maping:%p, sd %u\n", msg->to.idx, core,
              core->maping, sockid->sd);

        ret = corenet_attach(core->corenet, sockid, ctx, corerpc_recv,
                             corerpc_close, NULL, NULL, netable_rname(&ctx->coreid.nid));
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        sche_post(core->sche);

        return NULL;
err_ret:
        close(sockid->sd);
        return NULL;
}

static int __corenet_tcp_accept(const __corenet_tcp_t *corenet_tcp)
{
        int ret, sd;
        socklen_t alen;
        struct sockaddr_in sin;
        corerpc_ctx_t *ctx;

        memset(&sin, 0, sizeof(sin));
        alen = sizeof(struct sockaddr_in);

        sd = accept(corenet_tcp->sd, &sin, &alen);
        if (sd < 0 ) {
                ret = errno;
		GOTO(err_ret, ret);
        }

        ret = slab_static_alloc1((void **)&ctx, sizeof(*ctx));
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        ctx->running = 0;
        ctx->sockid.sd = sd;
        ctx->sockid.type = SOCKID_CORENET;
        ctx->sockid.seq = _random();
        ctx->sockid.addr = sin.sin_addr.s_addr;
        ctx->sockid.reply = corerpc_reply_tcp;
        ctx->coreid.nid.id = 0;
        ctx->local = corenet_tcp->coreid;

        ret = ltg_thread_create(__corenet_tcp_accept__, ctx, "__corenet_tcp_accept");
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        return 0;
err_ret:
        return ret;
}

static void *__corenet_tcp_passive(void *_arg)
{
        int ret;
        __corenet_tcp_t *corenet_tcp = _arg;

        DINFO("start...\n");

        main_loop_hold();

        while (1) {
                ret = sock_poll_sd(corenet_tcp->sd, 1000 * 1000, POLLIN);
                if (unlikely(ret)) {
                        if (ret == ETIMEDOUT || ret == ETIME)
                                continue;
                        else
                                GOTO(err_ret, ret);
                }

                DBUG("got new event\n");

                __corenet_tcp_accept(corenet_tcp);
        }

        return NULL;
err_ret:
        UNIMPLEMENTED(__DUMP__);
        return NULL;
}

int corenet_tcp_passive(const coreid_t *coreid, uint32_t *_port, int *_sd)
{
        int ret, sd, port;
        char tmp[MAX_LINE_LEN];

        port = LNET_PORT_RANDOM;
        while (srv_running) {
                port = (uint16_t)(LNET_SERVICE_BASE
                                  + (random() % LNET_SERVICE_RANGE));

                LTG_ASSERT(port > LNET_SERVICE_RANGE && port < 65535);
                snprintf(tmp, MAX_LINE_LEN, "%u", port);

                ret = tcp_sock_hostlisten(&sd, NULL, tmp,
                                          256, 0, 1);
                if (unlikely(ret)) {
                        if (ret == EADDRINUSE) {
                                DBUG("port (%u + %u) %s\n", LNET_SERVICE_BASE,
                                     port - LNET_SERVICE_BASE, strerror(ret));
                                continue;
                        } else
                                GOTO(err_ret, ret);
                } else {
                        break;
                }
        }

        *_port = port;
        *_sd = sd;

        __corenet_tcp_t *corenet_tcp;
        ret = ltg_malloc((void **)&corenet_tcp, sizeof(*corenet_tcp));
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        corenet_tcp->coreid = *coreid;
        corenet_tcp->sd = sd;

        ret = ltg_thread_create(__corenet_tcp_passive, corenet_tcp, "corenet_passive");
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        //DINFO("listen %u, nid %u\n", port, net_getnid()->id);

        return 0;
err_ret:
        return ret;
}
