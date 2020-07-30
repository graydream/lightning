

#include <limits.h>
#include <time.h>
#include <string.h>
#include <sys/epoll.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

typedef struct {
        int hash;
        nid_t from;
        nid_t to;
} corenet_msg_t;

#if 1
static int __rdma_connect_request(va_list ap)
{
        const uint32_t addr = va_arg(ap, const uint32_t);
        const uint32_t port = va_arg(ap, const uint32_t);
        core_t *core = va_arg(ap, core_t *);
        sockid_t *sockid = va_arg(ap, sockid_t *);

        va_end(ap);

        return corenet_rdma_connect_by_channel(addr, port, core, sockid);
}
#endif

int corenet_rdma_connect(uint32_t addr, uint32_t port, sockid_t *sockid)
{
        int ret;
        core_t *core = core_self();

        port = ntohs(port);

        DINFO("connect to %s:%d\n", _inet_ntoa(addr), port);

        ANALYSIS_BEGIN(0);

#if 1
        ret = sche_thread_solo(SCHE_THREAD_MISC, _random(), FALSE,
                               "rdma_connect", -1, __rdma_connect_request,
                               addr, port, core, sockid);
#else
        ret = corenet_rdma_connect_by_channel(addr, port, core, sockid);
#endif
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        ANALYSIS_END(0, 1000 * 1000 * 5, NULL);

        SOCKID_DUMP(sockid);

        return 0;
err_ret:
        ANALYSIS_END(0, 1000 * 1000 * 5, NULL);
        return ret;
}

static void * __corenet_passive_rdma(uint32_t *port, int cpu_idx)
{
        int ret;
        uint32_t _port;

retry:
        _port = (uint16_t)(LNET_SERVICE_BASE
                                 + (random() % LNET_SERVICE_RANGE));

        ret = corenet_rdma_listen_by_channel(cpu_idx, _port);
        if (ret) {
                if (ret == EAGAIN)
                     goto retry;

                GOTO(err_ret, ret);
        }

        *port = _port;

#if CORENET_RDMA_ON_ACTIVE_WAIT
        ret = corenet_rdma_on_passive_event(cpu_idx);
        if (ret)
                GOTO(err_ret, ret);
#endif

        return NULL;
err_ret:
        UNIMPLEMENTED(__DUMP__);
        return NULL;
}

int corenet_rdma_passive(uint32_t *port, int cpu_index)
{
        // test_device();

#if CORENET_RDMA_ON_ACTIVE_WAIT
        int ret;
        pthread_t th;
        pthread_attr_t ta;
        ret = pthread_create(&th, &ta, __corenet_passive_rdma, port, cpu_index);
        if (unlikely(ret))
                GOTO(err_ret, ret);
#else
        DINFO("cpu_index %d\n", cpu_index);
        __corenet_passive_rdma(port, cpu_index);
#endif

        return 0;
}
