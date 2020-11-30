#include <limits.h>
#include <time.h>
#include <string.h>
#include <sys/epoll.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <stdarg.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_CORE

#include "ltg_net.h"
#include "ltg_utils.h"
#include "ltg_rpc.h"
#include "ltg_core.h"
#include "ltg_net.h"

typedef struct {
        time_t last_update;
        uint64_t used;
        uint64_t count;
} core_latency_t;

static __thread core_latency_t *core_latency = NULL;

static int __core_latency_private_init(core_latency_t **_core_latency)
{
        int ret;

        LTG_ASSERT(core_latency == NULL);
        ret = ltg_malloc((void **)&core_latency, sizeof(*core_latency));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        memset(core_latency, 0x0, sizeof(*core_latency));
        *_core_latency = core_latency;

        return 0;
err_ret:
        return ret;
}

void core_latency_update(uint64_t used)
{
        if (unlikely(core_latency == NULL)) {
                return;
        }

        core_latency->used += used;
        core_latency->count++;

        time_t now = gettime();
        if (likely(now - core_latency->last_update < 2)) {
                return;
        }
        
        core_latency->used = core_latency->used / core_latency->count;
        core_latency->count = 1;
        core_latency->last_update = now;

        
        DBUG("latency %llu / %llu\n", (LLU)core_latency->used, (LLU)core_latency->count);
}

uint64_t S_LTG core_latency_get()
{
        if (likely(core_latency && core_latency->count)) {
                DBUG("latency %llu / %llu\n", (LLU)core_latency->used, (LLU)core_latency->count);
                return core_latency->used / core_latency->count;
        } else
                return 0;
}

static int __core_latency_init(va_list ap)
{
        int ret;
        core_t *core = core_self();
        core_latency_t *core_latency;

        va_end(ap);

        ret = __core_latency_private_init(&core_latency);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        DINFO("%s[%u] latency inited\n", core->name, core->hash);

        return 0;
err_ret:
        return ret;
}

int core_latency_init()
{
        int ret;

        ret = core_init_modules("core_latency", __core_latency_init, NULL);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        DINFO("core global latency inited\n");

        return 0;
err_ret:
        return ret;
}

