#include <pthread.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_UTILS

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_core.h"

int pspin_init(pspin_t *pspin)
{
        int ret;
        
        memset(pspin, 0x0, sizeof(*pspin));

#if PSPIN_DEBUG
        DWARN("PSPIN_DEBUG enabled\n");

        coreid_t coreid;
        ret = core_getid(&coreid);
        if (unlikely(ret))
                GOTO(err_ret, ret);
        pspin->coreid = coreid.idx;
#else
        (void) ret;
#endif

        return 0;
#if PSPIN_DEBUG
err_ret:
        return ret;
#endif
}

int pspin_destroy(pspin_t *pspin)
{
        int ret;

        LTG_ASSERT(pspin->locked == 0);
        
#if PSPIN_DEBUG
        coreid_t coreid;
        ret = core_getid(&coreid);
        if (unlikely(ret))
                GOTO(err_ret, ret);
        pspin->coreid = coreid.idx;
#else
        (void) ret;
#endif

        memset(pspin, 0x0, sizeof(*pspin));
        
        return 0;
#if PSPIN_DEBUG
err_ret:
        return ret;
#endif
}

int S_LTG pspin_lock(pspin_t *pspin)
{
        int ret;

#if PSPIN_DEBUG
        coreid_t coreid;
        ret = core_getid(&coreid);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        LTG_ASSERT(coreid.idx == pspin->coreid);
#else
        (void) ret;
#endif

        LTG_ASSERT(!pspin->locked);

        pspin->locked = 1;
        
        return 0;
#if PSPIN_DEBUG
err_ret:
        return ret;
#endif
}

int S_LTG pspin_trylock(pspin_t *pspin)
{
        if (pspin->locked) {
                return EBUSY;
        } else {
                return pspin_lock(pspin);
        }
}

int S_LTG pspin_locked(pspin_t *pspin)
{
        return pspin->locked;
}

int S_LTG pspin_unlock(pspin_t *pspin)
{
        int ret;

#if PSPIN_DEBUG
        coreid_t coreid;
        ret = core_getid(&coreid);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        LTG_ASSERT(coreid.idx == pspin->coreid);
#else
        (void) ret;
#endif

        LTG_ASSERT(pspin->locked);

        pspin->locked = 0;
        
        return 0;
#if PSPIN_DEBUG
err_ret:
        return ret;
#endif
}
