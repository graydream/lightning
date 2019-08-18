#include <limits.h>
#include <time.h>
#include <string.h>
#include <sys/epoll.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <netinet/tcp.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_net.h"
#include "ltg_utils.h"
#include "ltg_core.h"
#include "ltg_net.h"

static void IO_FUNC  __corenet_routine(void *_core, void *var, void *_corenet)
{
        (void) _core;
        (void) _corenet;

        if (likely(ltgconf_global.rdma)) {
                corenet_rdma_commit(((__corenet_t *)_corenet)->rdma_net);
        } else {
                corenet_tcp_check_add();
                corenet_tcp_commit(var);
        }

        return;
}

static void __corenet_scan(void *_core, void *var, void *_corenet)
{
        (void) _core;
        (void) _corenet;
        (void) var;

        if (unlikely(!ltgconf_global.rdma || ltgconf_global.tcp_discovery)) {
                corenet_tcp_check();
        }

        return;
}


static void IO_FUNC __corenet_poller(void *_core, void *var, void *_corenet)
{
        __corenet_t *corenet = _corenet;

        if (likely(ltgconf_global.rdma && ltgconf_global.daemon)) {
                corenet_rdma_poll(corenet);
        } else {
                core_t *core = _core;
                int tmo = (core->flag & CORE_FLAG_POLLING) ? 0 : 1;

                corenet_tcp_poll(var, tmo);
        }

        return;
}

#if 1
inline static void __core_interrupt_eventfd_func(void *arg)
{
        int ret;
        char buf[MAX_BUF_LEN];
        core_t *core = core_self();

        (void) arg;

        ret = read(core->interrupt_eventfd, buf, MAX_BUF_LEN);
        if (ret < 0) {
                ret = errno;
                LTG_ASSERT(ret == EAGAIN);
        }

        DBUG("interrupt_eventfd %d\n", core->interrupt_eventfd);
}
#endif

static int __corenet_tcp_init(core_t *core, __corenet_t *corenet, int flag)
{
        int ret;

        (void) core;
        (void) flag;

        ret = corenet_tcp_init(32768, (corenet_tcp_t **)&corenet->tcp_net);
        if (unlikely(ret))
                GOTO(err_ret, ret);

#if 1
        if (core->interrupt_eventfd != -1) {
                sockid_t sockid;
                sockid.sd = core->interrupt_eventfd;
                sockid.seq = _random();
                sockid.type = SOCKID_CORENET;
                sockid.addr = 123;
                ret = corenet_tcp_add(NULL, &sockid, NULL, NULL, NULL, NULL,
                                      __core_interrupt_eventfd_func, "interrupt_fd");
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }
#endif

        ret = corenet_tcp_passive(&corenet->coreid, &corenet->port,
                                  &corenet->sd);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

static int __corenet_rdma_init(__corenet_t *corenet, int flag)
{
        int ret;

        (void) flag;

        corenet->dev_count = 0;
        ret = corenet_rdma_init(32768, (corenet_rdma_t **)&corenet->rdma_net);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (ltgconf_global.daemon) {
                ret = corenet_rdma_passive(&corenet->port, corenet->coreid.idx);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

static int __corenet_init(va_list ap)
{
        int ret;
        __corenet_t *corenet;
        core_t *core = core_self();
        int *flag = va_arg(ap, int *);

        va_end(ap);

        ret = slab_static_alloc1((void **)&corenet, sizeof(*corenet));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (ltgconf_global.daemon) {
                ret = core_getid(&corenet->coreid);
                if (unlikely(ret))
                        GOTO(err_free, ret);
        }

        if (ltgconf_global.rdma && ltgconf_global.daemon) {
                ret = __corenet_rdma_init(corenet, *flag);
                if (unlikely(ret))
                        GOTO(err_free, ret);

        } else {
                ret = __corenet_tcp_init(core, corenet, *flag);
                if (unlikely(ret))
                        GOTO(err_free, ret);
        }

#if 1
        ret = core_register_poller("corenet_poller", __corenet_poller, corenet);
        if (unlikely(ret))
                GOTO(err_close, ret);

        ret = core_register_routine("corenet_routine", __corenet_routine, corenet);
        if (unlikely(ret))
                GOTO(err_close, ret);

        ret = core_register_scan("corenet_scan", __corenet_scan, corenet);
        if (unlikely(ret))
                GOTO(err_close, ret);
#endif

        core->corenet = corenet;

        return 0;
err_close:
        UNIMPLEMENTED(__DUMP__);
err_free:
        slab_static_free1((void **)&corenet);
err_ret:
        return ret;
}

int corenet_init(int flag)
{
        int ret;
#if ENABLE_RDMA
        ret = rdma_event_init();
        if (ret)
                GOTO(err_ret, ret);

        ret = corenet_rdma_evt_channel_init();
        if (ret)
                GOTO(err_ret, ret);
#endif
        ret = core_init_modules("corenet", __corenet_init, &flag, NULL);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

static int __corenet_getaddr(va_list ap)
{
        int ret;
        core_t *core = core_self();
        __corenet_t *corenet = core->corenet;
        corenet_addr_t *addr = va_arg(ap, corenet_addr_t *);

        va_end(ap);

        if (ltgconf_global.rdma && ltgconf_global.daemon) {
                ret = corenet_tcp_getaddr(corenet->port, addr);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        } else {
                ret = corenet_tcp_getaddr(corenet->port, addr);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

int corenet_getaddr(const coreid_t *coreid, corenet_addr_t *addr)
{
        int ret;

        ret = core_request(coreid->idx, -1, "corenet_gedaddr",
                           __corenet_getaddr, addr);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int corenet_attach(void *_corenet, const sockid_t *sockid, void *ctx,
                   core_exec exec, func_t reset, func_t check, func_t recv,
                   const char *name)
{
        __corenet_t *corenet = _corenet;

        if (ltgconf_global.rdma) {
                UNIMPLEMENTED(__DUMP__);
                return 0;
        } else {
                return corenet_tcp_add(corenet->tcp_net, sockid, ctx, exec,
                                       reset, check, recv, name);
        }
}


int corenet_send(void *ctx, const sockid_t *sockid, ltgbuf_t *buf)
{

        if (ltgconf_global.rdma) {
                UNIMPLEMENTED(__DUMP__);
                return 0;
        } else {
                return corenet_tcp_send(ctx, sockid, buf);
        }
}

void corenet_close(const sockid_t *sockid)
{
        if (ltgconf_global.rdma && sockid->rdma_handler != NULL) {
                corenet_rdma_close((rdma_conn_t *)sockid->rdma_handler);
        } else {
                corenet_tcp_close(sockid);
        }
}
