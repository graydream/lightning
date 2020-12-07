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

#define NETCTL_HASH 0

//static coremask_t __netctl_coremask__;
static uint64_t __mask__ = 0;

typedef struct {
        coremask_t mask;
        int numaid[CORE_MAX];
} netctl_t;

#if !NETCTL_HASH
static __thread int __cur__ = 0;
#endif

static netctl_t __netctl__;

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
        netctl_t *netctl;

        DINFO("netctl mask 0x%x\n", mask);

        __mask__ = mask;
        
        netctl = &__netctl__;
        
        coremask_trans(&netctl->mask, mask);

        for (int i = 0; i < netctl->mask.count; i++) {
                if (!ltgconf_global.numa) { 
                        netctl->numaid[i] = 0;
                        continue;
                }

                core_t *core =  core_get(netctl->mask.coreid[i]);
                LTG_ASSERT(core);

                if (core->main_core == NULL) {
                        netctl->numaid[i] = 0;
                        continue;
                }
                
                netctl->numaid[i] = core->main_core->node_id;
        }
        
        ret = core_init_modules1("netctl", mask,
                                 __register_ring_poller, NULL);
        if (ret)
                GOTO(err_ret, ret);

        core_occupy("netctl", mask);
        
        return 0;
err_ret:
        return ret;
}


int netctl_get(const coreid_t *coreid, coreid_t *_netctl)
{
        int numaid, retry = 0;
        netctl_t *netctl = &__netctl__;
        
        if (netctl->mask.count == 0) {
                return 0;
        }

        core_t *core = core_self();
        if (likely(ltgconf_global.numa && core->main_core)) {
                numaid = core->main_core->node_id;
        } else {
                numaid = 0;
        }
        
        *_netctl = *coreid;

        while (1) {
                __cur__ = (__cur__ + 1) % netctl->mask.count;

                if (netctl->numaid[__cur__] != numaid
                    && retry > netctl->mask.count) {
                        retry++;
                        continue;
                }
                
                _netctl->idx = netctl->mask.coreid[__cur__];

                break;
        }

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

