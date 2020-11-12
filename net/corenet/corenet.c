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

extern int ltg_nofile_max;

static void IO_FUNC  __corenet_routine(void *_core, void *var, void *_corenet)
{
        (void) _core;
        (void) _corenet;

        if (likely(ltgconf_global.rdma)) {
                corenet_rdma_commit(((__corenet_t *)_corenet)->rdma_net);
        } else {
                corenet_tcp_commit(var);
        }

        return;
}

static void __corenet_scan(void *_core, void *var, void *_corenet)
{
        (void) _core;
        (void) _corenet;
        (void) var;

        if (unlikely(!ltgconf_global.rdma)) {
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

        ret = corenet_tcp_init(ltg_nofile_max, (corenet_tcp_t **)&corenet->tcp_net);
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
        ret = corenet_rdma_init(ltg_nofile_max, (corenet_rdma_t **)&corenet->rdma_net);
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
                ret = __corenet_rdma_init(corenet, 0);
                if (unlikely(ret))
                        GOTO(err_free, ret);
        } else {
                ret = __corenet_tcp_init(core, corenet, 0);
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

int corenet_init(uint64_t mask)
{
        int ret;
#if ENABLE_RDMA
        if (ltgconf_global.rdma && ltgconf_global.daemon) {
                ret = rdma_event_init();
                if (ret)
                        GOTO(err_ret, ret);

                ret = corenet_rdma_evt_channel_init();
                if (ret)
                        GOTO(err_ret, ret);
        }
#endif

        ret = core_init_modules1("corenet", mask, __corenet_init, NULL);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

#define NETINFO_TIMEOUT (10 * 60)

static int __corenet_get_numaid(uint32_t addr, int *numaid)
{
        int ret;
        char name[MAX_NAME_LEN], path[MAX_PATH_LEN], value[MAX_BUF_LEN];
        
        ret = tcp_sock_getdevice(addr, name);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        snprintf(path, MAX_LINE_LEN, "/sys/class/net/%s/device/numa_node", name);
        ret = _get_text(path, value, MAX_BUF_LEN);
        if (ret < 0) {
                ret = -ret;
                GOTO(err_ret, ret);
        }

        DBUG("addr %s device %s numa node %s", _inet_ntoa(addr), name, value);

        *numaid = atoi(value);
        
        return 0;
err_ret:
        return ret;
}

static int __corenet_getaddr____(const core_t *core, uint32_t port,
                                 corenet_addr_t *addr, int force)
{
        int ret, numaid;
        char *buf = slab_stream_alloc(PAGE_SIZE);
        ltg_net_info_t *info;
        uint32_t buflen = MAX_BUF_LEN;

        LTG_ASSERT(core);
        ret = core_getid(&addr->coreid);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        info = (ltg_net_info_t *)buf;
        ret = net_getinfo(buf, &buflen, port, &ltg_netconf_global);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        int count = 0;
        for (int i = 0; i < info->info_count; i++) {
                if (force) {
                        DBUG("%s[%u] use cross core addr %s:%u\n", core->name,
                             addr->coreid.idx, _inet_ntoa(info->info[i].addr),
                             info->info[i].port);

                        addr->info[count] = info->info[i];
                        count++;
                } else {
                        ret = __corenet_get_numaid(info->info[i].addr, &numaid);
                        if (ret)
                                continue;

                        if (force == 0 && core->main_core
                            && numaid != core->main_core->node_id) {
                                DBUG("%s[%u] skip addr %s:%u\n", core->name,
                                      addr->coreid.idx, _inet_ntoa(info->info[i].addr),
                                      info->info[i].port);
                        } else {
                                DBUG("%s[%u] use addr %s:%u\n", core->name,
                                      addr->coreid.idx, _inet_ntoa(info->info[i].addr),
                                      info->info[i].port);

                                addr->info[count] = info->info[i];
                                count++;
                        }
                }
        }

        addr->info_count = count;
        addr->len = sizeof(*addr) + sizeof(sock_info_t) * count;

        if (count == 0) {
                ret = ENODEV;
                GOTO(err_ret, ret);
        }
        
        slab_stream_free(buf);

        return 0;
err_ret:
        slab_stream_free(buf);
        return ret;
}

int __corenet_getaddr__(uint32_t port, corenet_addr_t *addr)
{
        int ret;
        core_t *core = core_self();

        if (ltgconf_global.numa) {
                ret = __corenet_getaddr____(core, port, addr, 0);
                if (ret) {
                        if (ret == ENODEV) {
                                ret = __corenet_getaddr____(core, port, addr, 1);
                                if (ret)
                                        GOTO(err_ret, ret);
                        } else {
                                GOTO(err_ret, ret);
                        }
                }
        } else {
                ret = __corenet_getaddr____(core, port, addr, 1);
                if (ret)
                        GOTO(err_ret, ret);
        }
        
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

        ret = __corenet_getaddr__(corenet->port, addr);
        if (unlikely(ret))
                GOTO(err_ret, ret);

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
                corenet_rdma_close((rdma_conn_t *)sockid->rdma_handler, __FUNCTION__);
        } else {
                corenet_tcp_close(sockid);
        }
}

static uint64_t __mask__ = 0;

static void *__corenet_register(void *_mask)
{
        (void) _mask;
        
        while (srv_running) {
                sleep(5);
#if 0
                corenet_maping_offline(__mask__);
#else
                corenet_maping_register(__mask__);
#endif
        }

        pthread_exit(NULL);
}

int corenet_register(uint64_t coremask)
{
        int ret;
        nid_t nid = *net_getnid();
        char key[MAX_PATH_LEN];

        snprintf(key, MAX_NAME_LEN, "%d/coremask", nid.id);
        ret = etcd_create(ETCD_CORENET, key, (void *)&coremask,
                          sizeof(coremask), -1);
        if (unlikely(ret)) {
                ret = etcd_update(ETCD_CORENET, key, (void *)&coremask,
                                  sizeof(coremask), NULL, -1);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }
        
        __mask__ = coremask;

        ret = corenet_maping_register(__mask__);
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        ret = ltg_thread_create(__corenet_register, NULL, "corenet register");
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}
