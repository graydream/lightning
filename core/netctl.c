#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <semaphore.h>
#include <errno.h>
#include <sys/statfs.h>

#define DBG_SUBSYS  S_LTG_CORE

#include "ltg_rpc.h"
#include "ltg_utils.h"
#include "ltg_core.h"
#include "ltg_net.h"
#include "ltg_utils.h"

static coremask_t __netctl_coremask__;
static uint64_t __mask__ = 0;
static __thread int __cur__ = 0;

static int __register_ring_poller(va_list ap)
{
        int ret;
        va_end(ap);

        ret = core_register_poller("core_ring_poller", core_ring_poller, NULL);
        if(ret)
                UNIMPLEMENTED(__DUMP__);

        return 0;
}

int netctl_init(uint64_t mask)
{
        int ret;

        DINFO("netctl mask 0x%x\n", mask);

        ret = core_init_modules1("netctl", mask,
                                 __register_ring_poller, NULL);
        if (ret)
                GOTO(err_ret, ret);

        coremask_trans(&__netctl_coremask__, mask);
        __mask__ = mask;
        
        return 0;
err_ret:
        return ret;
}


int netctl_get(const coreid_t *coreid, coreid_t *netctl)
{
        if (__netctl_coremask__.count == 0) {
                return 0;
        }

        *netctl = *coreid;
        netctl->idx = __netctl_coremask__.coreid[__cur__];
        __cur__ = (__cur__ + 1) % __netctl_coremask__.count;

        return 1;
}

int netctl()
{
        int ret;
        coreid_t coreid;

        ret = core_getid(&coreid);
        if (unlikely(ret))
                return 0;

        return core_usedby(__mask__, coreid.idx);
}

STATIC int IO_FUNC __netctl_postwait(va_list ap)
{
        const coreid_t *netctl = va_arg(ap, const coreid_t *);
        const char *name = va_arg(ap, const char *);
        const coreid_t *coreid = va_arg(ap, const coreid_t *);
        const void *request = va_arg(ap, const void *);
        int reqlen = va_arg(ap, int);
        int replen = va_arg(ap, int);
        const ltgbuf_t *wbuf = va_arg(ap, const ltgbuf_t *);
        ltgbuf_t *rbuf = va_arg(ap, ltgbuf_t *);
        int msg_type = va_arg(ap, int);
        int msg_size = va_arg(ap, int);
        int group = va_arg(ap, int);
        int timeout = va_arg(ap, int);

        va_end(ap);

        (void) netctl;

        DBUG("%s redirect to netctl\n", name);
        
        return corerpc_postwait(name, netctl, coreid, request, reqlen, replen,
                                wbuf, rbuf, msg_type, msg_size,
                                group, timeout);
}

int IO_FUNC netctl_postwait(const char *name, const coreid_t *coreid, const void *request,
                            int reqlen, int replen, const ltgbuf_t *wbuf, ltgbuf_t *rbuf,
                            int msg_type, int msg_size, int group, int timeout)
{
        coreid_t netctl;

        if (netctl_get(coreid, &netctl)) {
                DBUG("%s redirect to netctl\n", name);
                return core_ring_wait(netctl.idx, -1, name, __netctl_postwait, &netctl,
                                      name, coreid, request, reqlen, replen,
                                      wbuf, rbuf, msg_type, msg_size,
                                      group, timeout);
        } else {
                return corerpc_postwait(name, NULL, coreid, request, reqlen, replen,
                                        wbuf, rbuf, msg_type, msg_size,
                                        group, timeout);
        }
}

STATIC int IO_FUNC __netctl_postwait1(va_list ap)
{
        const coreid_t *netctl = va_arg(ap, const coreid_t *);
        const char *name = va_arg(ap, const char *);
        const coreid_t *coreid = va_arg(ap, const coreid_t *);
        const void *request = va_arg(ap, const void *);
        int reqlen = va_arg(ap, int);
        void *reply = va_arg(ap, void *);
        int *replen = va_arg(ap, int *);
        int msg_type = va_arg(ap, int);
        int group = va_arg(ap, int);
        int timeout = va_arg(ap, int);

        va_end(ap);

        (void) netctl;

        DBUG("%s redirect to netctl\n", name);
        
        return corerpc_postwait1(name, netctl, coreid, request, reqlen,
                                 reply, replen, msg_type,
                                 group, timeout);
}


int IO_FUNC netctl_postwait1(const char *name, const coreid_t *coreid,
                             const void *request, int reqlen,  void *reply,
                             int *replen, int msg_type, int group, int timeout)
{
        coreid_t netctl;

        if (netctl_get(coreid, &netctl)) {
                DBUG("%s redirect to netctl\n", name);
                return core_ring_wait(netctl.idx, -1, name, __netctl_postwait1, &netctl,
                                      name, coreid, request, reqlen,
                                      reply, replen, msg_type,
                                      group, timeout);
        } else {
                return corerpc_postwait1(name, NULL, coreid, request, reqlen,
                                         reply, replen, msg_type,
                                         group, timeout);
        }
}
