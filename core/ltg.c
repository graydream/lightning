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
#include "utils/nodeid.h"
#include "ltg.h"

struct ltgconf_t ltgconf;
ltg_netconf_t ltg_netconf;
ltg_global_t ltg_global;
int ltg_nofile_max = 0;

extern analysis_t *default_analysis;

#define XMITBUF (1024 * 1024 * 100)     /* 100MB */

static int __get_nofailmax(int *nofilemax)
{
        int ret;
        struct rlimit rlim_new;

        ret = getrlimit(RLIMIT_NOFILE, &rlim_new);
        if (ret == -1) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        *nofilemax = rlim_new.rlim_max;

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


int ltg_conf_init(const char *sysname, const char *srv_name, const char *workdir,
                  uint64_t coremask, int rpc_timeout, int polling_timeout, int rdma,
                  int performance_analysis, int use_huge,
                  int backtrace, int daemon, int coreflag, int tls)
{
        int ret;

#if 1
        if ((coreflag & CORE_FLAG_POLLING) == 0) {
                use_huge = 0;
        }
#endif
        __ltg_global_init();
        
        ret = __get_nofailmax(&ltg_nofile_max);
        if (ret)
                GOTO(err_ret, ret);

        memset(&ltgconf, 0x0, sizeof(ltgconf));

        strcpy(ltgconf.system_name, sysname);
        strcpy(ltgconf.service_name, srv_name);
        if (workdir) {
                strcpy(ltgconf.workdir, workdir);
        } else {
                LTG_ASSERT(!daemon);
                ltgconf.workdir[0] = '\0';
        }

        ltgconf.maxcore = 1;
        ltgconf.rdma = rdma;
        ltgconf.rpc_timeout = rpc_timeout;
        ltgconf.performance_analysis = performance_analysis;
        ltgconf.lease_timeout = _max(3, rpc_timeout / 3);
        ltgconf.hb_timeout = _max(3, rpc_timeout / 3);
        ltgconf.hb_retry = 2;
        ltgconf.coredump = 1;
        ltgconf.backtrace = backtrace;
        ltgconf.polling_timeout = polling_timeout;
        ltgconf.use_huge = use_huge;
        ltgconf.daemon = daemon;
        ltgconf.coreflag = coreflag;
        ltgconf.tls = tls;

        
        ltgconf.coremask = coremask;

        ltgconf.wmem_max = XMITBUF;
        ltgconf.rmem_max = XMITBUF;

        memset(&ltg_netconf, 0x0, sizeof(ltg_netconf));

        return 0;
err_ret:
        return ret;
}

static int __nodeid_init(const char *name)
{
        int ret;
        nodeid_t id;

        ret = nodeid_load(&id);
        if (ret) {
                if (ret == ENOENT) {
                        ret = nodeid_init(&id, name);
                        if (ret)
                                GOTO(err_ret, ret);

                } else
                        GOTO(err_ret, ret);
        }

        nid_t nid;
        nid.id = id;
        net_setnid(&nid);
        
        return 0;
err_ret:
        return ret;
}

static int __ltg_init_stage1(const char *name)
{
        int ret;

        fnotify_init();
        dmsg_init(ltgconf.system_name);

        if (ltgconf.daemon) {
                ret = __nodeid_init(name);
                if (ret)
                        GOTO(err_ret, ret);
        }

        ret = sche_init();
        if (unlikely(ret))
                GOTO(err_ret, ret);

        analysis_init();

        if (ltgconf.performance_analysis) {
                ret = analysis_create(&default_analysis, "default", 0);
                if (unlikely(ret))             
                        GOTO(err_ret, ret);            
        }
        
        ret = core_init(ltgconf.coremask, ltgconf.tls, ltgconf.coreflag);
        if (ret)
                GOTO(err_ret, ret);

        ret = etcd_init();
        if (ret)
                GOTO(err_ret, ret);

        ret = maping_init();
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

        int thread = ltgconf.daemon ? 5 : 2;
        ret = main_loop_create(thread);
        if (ret)
                GOTO(err_ret, ret);
        
        ret = rpc_init(name);
        if (ret)
                GOTO(err_ret, ret);

        if (ltgconf.daemon) {
                ret = rpc_passive(-1);
                if (ret)
                        GOTO(err_ret, ret);
        }

        ret = network_init();
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (ltgconf.daemon) {
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

int ltg_init(const char *name)
{
        int ret;

        ret = __ltg_init_stage1(name);
        if (ret)
                GOTO(err_ret, ret);

        ret = __ltg_init_stage2(name);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

void ltg_net_add(uint32_t network, uint32_t mask)
{
        LTG_ASSERT(ltg_netconf.count + 1 < MAX_NET_COUNT);

        ltg_netconf.network[ltg_netconf.count].network = network;
        ltg_netconf.network[ltg_netconf.count].mask = mask;
        ltg_netconf.count ++;
}
