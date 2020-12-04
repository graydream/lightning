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

        core_occupy("netctl", mask);
        
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

