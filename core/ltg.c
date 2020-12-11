#include <limits.h>
#include <time.h>
#include <string.h>
#include <sys/epoll.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <stdarg.h>
#include <sys/resource.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_CORE

#include "ltg_net.h"
#include "ltg_utils.h"
#include "ltg_core.h"
#include "ltg_lib.h"

ltgconf_t ltgconf_global;
ltg_netconf_t ltg_netconf_global;
ltg_netconf_t ltg_netconf_manage;
ltg_global_t ltg_global;
int ltg_nofile_max = 0;

extern analysis_t *default_analysis;

#define XMITBUF (1024 * 1024 * 100)     /* 100MB */

static int __get_nofilemax(int daemon, int *nofilemax)
{
        int ret;
        struct rlimit rlim_new;

        (void) daemon;
        
        if (daemon) {
                rlim_new.rlim_cur = ltgconf_global.nofile_max;
                rlim_new.rlim_max = ltgconf_global.nofile_max;
                ret = setrlimit(RLIMIT_NOFILE, &rlim_new);
                if (ret == -1) {
                        ret = errno;
                        GOTO(err_ret, ret);
                }
        }
        
        ret = getrlimit(RLIMIT_NOFILE, &rlim_new);
        if (ret == -1) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        *nofilemax = rlim_new.rlim_max;

        DINFO("max open file %d\n", *nofilemax);
        
        return 0;
err_ret:
        return ret;
}

static void __ltg_global_init()
{
        memset(&ltg_global, 0x0, sizeof(ltg_global_t));

        ltg_global.master_magic = 1;
        UNIMPLEMENTED(__NULL__);
}


int ltg_conf_init(ltgconf_t *ltgconf, const char *system_name)
{
        __ltg_global_init();

        memset(ltgconf, 0x0, sizeof(*ltgconf));

        ltgconf->hb_retry = 2;
        ltgconf->coredump = 1;
        ltgconf->wmem_max = XMITBUF;
        ltgconf->rmem_max = XMITBUF;

        memset(&ltg_netconf_global, 0x0, sizeof(ltg_netconf_global));
        memset(&ltg_netconf_manage, 0x0, sizeof(ltg_netconf_manage));

        strcpy(ltgconf->system_name, system_name);
        strcpy(ltgconf_global.system_name, system_name);
        
        return 0;
}

static int __ltg_init_stage1(const nid_t *nid, const char *name)
{
        int ret;

        (void) name;

        ret = __get_nofilemax(ltgconf_global.daemon, &ltg_nofile_max);
        if (ret)
                GOTO(err_ret, ret);
        
        ret = fnotify_init();
        if (ret)
                GOTO(err_ret, ret);

        ret = dmsg_init(ltgconf_global.system_name);
        if (ret)
                GOTO(err_ret, ret);

        seg_init();
        
        if (ltgconf_global.daemon) {
                net_setnid(nid);
        }

        ret = sche_init();
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = core_init(ltgconf_global.coremask, ltgconf_global.coreflag);
        if (ret)
                GOTO(err_ret, ret);

        ret = analysis_init();
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        ret = timer_init(ltgconf_global.coreflag & CORE_FLAG_POLLING);
        if (ret)
                GOTO(err_ret, ret);
        
        ret = etcd_init();
        if (ret)
                GOTO(err_ret, ret);
        
        DINFO("stage1 inited\n");
        
        return 0;
err_ret:
        return ret;
}

static int __ltg_init_stage2(const char *name)
{
        int ret;

        int thread = ltgconf_global.daemon ? 5 : 2;
        ret = main_loop_create(thread);
        if (ret)
                GOTO(err_ret, ret);
        
        ret = rpc_init(name);
        if (ret)
                GOTO(err_ret, ret);

        if (ltgconf_global.daemon) {
                ret = rpc_passive(-1);
                if (ret)
                        GOTO(err_ret, ret);

                ret = conn_init();
                if (ret)
                        GOTO(err_ret, ret);
        }

        main_loop_start();
        
        DINFO("stage2 inited\n");
        
        return 0;
err_ret:
        return ret;
}

int ltg_init(const ltgconf_t *ltgconf, const ltg_netconf_t *ltgnet_manage,
             const ltg_netconf_t *ltgnet_conf)
{
        int ret;

#if SCHEDULE_TASKCTX_RUNTIME
        init_global_hz();
#endif
        memcpy(&ltgconf_global, ltgconf, sizeof(*ltgconf));

        ltg_netconf_global.count = 0;

        for (int i = 0; i < ltgnet_conf->count; i++) {
                ltg_netconf_global.network[i].network
                        = ltgnet_conf->network[i].network;
                ltg_netconf_global.network[i].mask
                        = ltgnet_conf->network[i].mask;
                ltg_netconf_global.count++;
        }

        ltg_netconf_manage.count = 0;
        for (int i = 0; i < ltgnet_manage->count; i++) {
                ltg_netconf_manage.network[i].network
                        = ltgnet_manage->network[i].network;
                ltg_netconf_manage.network[i].mask
                        = ltgnet_manage->network[i].mask;
                ltg_netconf_manage.count++;
        }
        
        ret = __ltg_init_stage1(&ltgconf->nid, ltgconf_global.service_name);
        if (ret)
                GOTO(err_ret, ret);

        ret = __ltg_init_stage2(ltgconf_global.service_name);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

void ltg_net_add(uint32_t network, uint32_t mask)
{
        LTG_ASSERT(ltg_netconf_global.count + 1 < MAX_NET_COUNT);

        ltg_netconf_global.network[ltg_netconf_global.count].network = network;
        ltg_netconf_global.network[ltg_netconf_global.count].mask = mask;
        ltg_netconf_global.count ++;
}
