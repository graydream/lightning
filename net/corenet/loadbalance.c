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

#include "ltg_utils.h"
#include "ltg_core.h"
#include "ltg_net.h"
#include "ltg_rpc.h"

#define __RANDOM__ (1024 * 16)

typedef struct {
        int ridx;
        int lidx;
        uint64_t latency[NODEID_MAX][CORE_MAX];
} loadbalance_t;

static uint64_t *__rand__ = NULL;

static int __loadbalance_init(va_list ap)
{
        int ret;
        loadbalance_t *loadbalance;

        va_end(ap);

        ret = ltg_malloc((void **)&loadbalance, sizeof(*loadbalance));
        if (ret)
                GOTO(err_ret, ret);

        memset(loadbalance, 0x0, sizeof(*loadbalance));
        DINFO("balance array size %d\n", sizeof(*loadbalance));

        loadbalance->ridx = _random();
        loadbalance->lidx = _random();
        
        core_tls_set(VARIABLE_LOADBALANCE, loadbalance);

        return 0;
err_ret:
        return ret;
}

static int __loadbalance_random_init()
{
        int ret, i;
        uint64_t *rand;

        ANALYSIS_BEGIN(0);
        ret = ltg_malloc((void**)&rand, sizeof(*rand) * __RANDOM__);
        if (ret)
                GOTO(err_ret, ret);

        for (i = 0; i < __RANDOM__; i++) {
                rand[i] = _random();
        }

        ANALYSIS_END(0, 1000 * 100, NULL);

        __rand__ = rand;
        
        return 0;
err_ret:
        return ret;
}

static uint64_t __loadbalance_random(loadbalance_t *loadbalance)
{
        int idx = (loadbalance->ridx++) + (loadbalance->lidx++);
        
        return __rand__[idx  % __RANDOM__];
}


int loadbalance_init()
{
        int ret;

        ret = __loadbalance_random_init();
        if (ret)
                GOTO(err_ret, ret);
        
        ret = core_init_modules("core_loadbalance", __loadbalance_init, NULL);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

void loadbalance_update(const coreid_t *coreid, uint64_t latency)
{
        loadbalance_t *loadbalance = core_tls_get(NULL, VARIABLE_LOADBALANCE);

        LTG_ASSERT(loadbalance);

        DBUG("update %s/%d latency %d\n", netable_rname(&coreid->nid),
              coreid->idx, latency);
        
        loadbalance->latency[coreid->nid.id][coreid->idx] = latency;
}

typedef struct {
        uint64_t latency;
        coreid_t coreid;
} section_t;

static int __loadbalance_get(loadbalance_t *loadbalance, const section_t *sec, int count)
{
        int i;
        uint64_t rand = 0;
        double loadinfo[32], max;

        max = 0;
        loadinfo[0] = 0;
        for (i = 1; i <= count; i++) {
                loadinfo[i] = sec[i - 1].latency;
                if (loadinfo[i] < 10)
                        loadinfo[i] = 10;

                max = max > loadinfo[i] ? max : loadinfo[i];
        }

        for (i = 1; i <= count; i++) {
                loadinfo[i] =  ((max  * 1000 * 1000) / loadinfo[i]);
        }

        for (i = 1; i <= count; i++) {
                loadinfo[i] = loadinfo[i] + loadinfo[i - 1];
        }

        rand = __loadbalance_random(loadbalance) % (int)(loadinfo[count]);

        for (i = 0; i < count; i++) {
                if (rand >= loadinfo[i] && rand < loadinfo[i + 1]) {
                        return i;
                }
        }

        return 0;
}

int IO_FUNC loadbalance_get(const coreid_t *array, int count)
{
        section_t section[32], *sec;
        loadbalance_t *loadbalance = core_tls_get(NULL, VARIABLE_LOADBALANCE);
        static int seq = 0;
        const coreid_t *coreid;
        
        if (unlikely(loadbalance == NULL)) {
                return seq++ % count;
        }

        for (int i = 0; i < count; i++) {
                coreid = &array[i];
                sec = &section[i];
                sec->coreid = *coreid;

                if (core_islocal(coreid)) {
                        sec->latency = core_latency_get();
                } else {
                        sec->latency = loadbalance->latency[coreid->nid.id][coreid->idx];  
                }

                DBUG("get %s/%d latency %d\n", netable_rname(&coreid->nid),
                      coreid->idx, sec->latency);
        }

        int idx = __loadbalance_get(loadbalance, section, count);

        DBUG("balance %d/%d\n", idx, count);

        return idx;
}
