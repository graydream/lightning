#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <openssl/md5.h>
#include <openssl/aes.h>
#include <dirent.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_net.h"
#include "ltg_utils.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

static int __network_connect_exec(const nid_t *nid, int force)
{
        int ret, retry = 0;
        net_handle_t nh;
        char buf[MAX_BUF_LEN];
        ltg_net_info_t *info;

        ANALYSIS_BEGIN(0);

        DBUG("connect to %s\n", netable_rname_nid(nid));

        if (netable_connected(nid)) {
                DINFO("%s already connected\n", netable_rname_nid(nid));
                return 0;
        }

        ANALYSIS_END(0, IO_WARN, NULL);
retry:
        info = (void *)buf;
        ret = maping_nid2netinfo(nid, info);
        if (unlikely(ret)) {
                if (ret == ENOENT || ret == ENOKEY) {
                        DBUG("net "NID_FORMAT" ret %u\n", NID_ARG(nid), ret);
                        ret = ENONET;
                        GOTO(err_ret, ret);
                } else if (ret == EAGAIN || ret == ENOSYS) {
                        USLEEP_RETRY(err_ret, ret, retry, retry, 3, (100 * 1000));
                } else
                        GOTO(err_ret, ret);
        }

        ANALYSIS_END(0, IO_WARN, NULL);

        LTG_ASSERT(info->len < MAX_BUF_LEN);

        ret = netable_connect_info(&nh, info, force);
        if (unlikely(ret)) {
                if (ret == ENONET) {
                        DBUG("connect %s ret %u\n", info->name, ret);
                        GOTO(err_ret, ret);
                } else if (ret == ENOKEY) {
                        DINFO("no such key, drop %u\n", nid->id);
                        char tmp[MAX_BUF_LEN];
                        snprintf(tmp, MAX_PATH_LEN, NID_FORMAT, NID_ARG(nid));
                        maping_drop(NID2NETINFO, tmp);
                        goto retry;
                } else
                        GOTO(err_ret, ret);
        }

        DINFO("%s connected\n", info->name);

        ANALYSIS_END(0, IO_WARN, NULL);

        return 0;
err_ret:
        return ret;
}

typedef struct {
        int pipe[2];
        nid_t nid;
        int force;
} conn_arg_t;

static void *__network_connect__(void *_arg)
{
        conn_arg_t *arg = _arg;
        int retval;

        retval = __network_connect_exec(&arg->nid, arg->force);
        write(arg->pipe[1], &retval, sizeof(retval));
        close(arg->pipe[1]);
        ltg_free((void **)&arg);

        pthread_exit(NULL);
}

static int __network_connect_wait_thread(const nid_t *nid, int force, int timeout)
{
        int ret, out, retval;
        pthread_t th;
        pthread_attr_t ta;
        conn_arg_t *arg;

        ANALYSIS_BEGIN(0);
        
        ret = ltg_malloc((void **)&arg, sizeof(*arg));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        arg->nid = *nid;
        arg->force = force;
        ret = pipe(arg->pipe);
        if (unlikely(ret))
                GOTO(err_free, ret);

        out = arg->pipe[0];
        
        (void) pthread_attr_init(&ta);
        (void) pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);

        ret = pthread_create(&th, &ta, __network_connect__, (void *)arg);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        if (timeout) {
                ret = sock_poll_sd(out, (1000 * 1000) * timeout, POLLIN);
                if (unlikely(ret)) {
                        close(out);
                        DWARN("connect %s timeout\n", network_rname(nid));
                        GOTO(err_ret, ret);
                }

                ret = read(out, &retval, sizeof(retval));
                if (unlikely(ret < 0)) {
                        UNIMPLEMENTED(__DUMP__);
                }

                close(out);
                
                if (retval) {
                        ret = retval;
                        GOTO(err_ret, ret);
                }

                
                if (!netable_connected(nid)) {
                        ret = ENONET;
                        GOTO(err_ret, ret);
                }

                DINFO("connect finish\n");
        } else {
                close(out);
                ret = ENONET;
                GOTO(err_ret, ret);
        }

        ANALYSIS_END(0, IO_WARN * (timeout + 1), NULL);
        
        return 0;
err_free:
        ltg_free((void **)&arg);
err_ret:
        ANALYSIS_END(0, IO_WARN * (timeout + 1), NULL);
        return ret;
}

static int __network_connect_exec_sche(va_list ap)
{
        const nid_t *nid = va_arg(ap, const  nid_t *);
        int force = va_arg(ap, int);
        int timeout = va_arg(ap, int);

        va_end(ap);

        return __network_connect_wait_thread(nid, force, timeout);
}


static int __network_connect_wait(const nid_t *nid, int timeout, int force)
{
        int ret;

        if (!netable_connectable(nid, force)) {
                ret = EAGAIN;
                DWARN("%s need try again \n", netable_rname_nid(nid));
                GOTO(err_ret, ret);
        }

        DINFO("try to connect %s \n", netable_rname_nid(nid));

        netable_update_retry(nid);

        if (sche_running()) {
                ret = sche_newthread(SCHE_THREAD_MISC, _random(), FALSE,
                                         "network_connect", -1,
                                         __network_connect_exec_sche,
                                         nid, force, timeout);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        } else {
                ret = __network_connect_wait_thread(nid, force, timeout);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

int network_ltime(const nid_t *nid, time_t *ltime)
{
        return network_connect(nid, ltime, 0, 0);
}

time_t network_ltime1(const nid_t *nid)
{
        int ret;
        time_t ltime;

        ret = network_connect(nid, &ltime, 0, 0);
        if (unlikely(ret)) {
                ltime = 0;
        }

        return ltime;
}

int IO_FUNC network_connect(const nid_t *nid, time_t *_ltime, int _timeout, int _force)
{
        int ret, force, timeout;
        time_t ltime;

        LTG_ASSERT(nid->id > 0);

retry:
        ltime = netable_conn_time(nid);
        if (likely(ltime != 0)) {
                if (_ltime)
                        *_ltime = ltime;
        
                goto out;
        }

        if (ltgconf.daemon == 0) {
#if 0
                instat_t instat;
                ret = mds_rpc_getstat(nid, &instat);
                if (ret)
                        GOTO(err_ret, ret);

                if (!instat.online) {
                        ret = ENONET;
                        GOTO(err_ret, ret);
                }
#endif

                timeout = 10;
                force = 1;
        } else if (net_islocal(nid)) {
                timeout = 10;
                force = 1;
        } else {
                timeout = _timeout;
                force = _force;
        }

        if (unlikely(timeout)) {
                ret = __network_connect_wait(nid, timeout, force);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                goto retry;
        } else {
                ret = ENONET;
                goto err_ret;
        }

        LTG_ASSERT(ltime);
        
out:
        return 0;
err_ret:
        return _errno_net(ret);
}

const char *network_rname(const nid_t *nid)
{
        return netable_rname_nid(nid);
}

int network_rname1(const nid_t *nid, char *name)
{
        int ret;

        network_connect(nid, NULL, 1, 0);
        
        ret = netable_rname1(nid, name);
        if (ret) {
                ret = maping_nid2host(nid, name);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }

        DBUG("%u %s\n", nid->id, name);
        
        return 0;
err_ret:
        return ret;
}

void network_close(const nid_t *nid, const char *why, const time_t *ltime)
{
        netable_close(nid, why, ltime);
}

int network_init()
{
        return 0;
}
