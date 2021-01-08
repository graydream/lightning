#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_core.h"

extern int ltg_nofile_max;

typedef struct {
        struct list_head hook;
        int type;
        int retval;
        task_t task;
} conn_wait_ctx_t;

typedef enum {
        TYPE_TASK = 100,
        TYPE_QUEUE,
} conn_wait_type_t;

int corenet_hb_add(const coreid_t *coreid, const sockid_t *sockid);

void coreid_check(int _coreid)
{
        int ret;
        coreid_t coreid;

        ret = core_getid(&coreid);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);
        
        LTG_ASSERT(_coreid == coreid.idx);
}

static void __corenet_maping_close_entry(corenet_maping_t *entry,
                                         const sockid_t *_sockid);

static  corenet_maping_t *__corenet_maping_get__()
{
        return core_tls_get(NULL, VARIABLE_MAPING);
}

static corenet_maping_t S_LTG *__corenet_maping_get_byctx(void *core)
{
        return core_tls_get(core, VARIABLE_MAPING);
}

static void __corenet_maping_resume(struct list_head *list, const nid_t *nid,
                                    int res)
{
        struct list_head *pos, *n;
        conn_wait_ctx_t *ctx;

        (void) nid;
        
        list_for_each_safe(pos, n, list) {
                ctx = (conn_wait_ctx_t *)pos;
                ctx->retval = res;

                if (ctx->type == TYPE_TASK) {
                        sche_task_post(&ctx->task, res, NULL);
                } else if (ctx->type == TYPE_QUEUE) {
                        //nothing to do here;
                }

                list_del_init(&ctx->hook);
        }
}

STATIC void __corenet_maping_close_finally__(const nid_t *nid, const sockid_t *sockid)
{
        (void) nid;
        
        if (ltgconf_global.rdma && sockid->rdma_handler != NULL)
                corenet_rdma_close((rdma_conn_t *)sockid->rdma_handler,
                                   __FUNCTION__);
        else
                corenet_tcp_close(sockid);

}

STATIC int __corenet_maping_connect_core(const coreid_t *coreid,
                                         const corenet_addr_t *addr,
                                         sockid_t *_sockid)
{
        int ret, idx, i;
        sockid_t sockid;
        const sock_info_t *sock;

        idx = _random() % addr->info_count;

        for (i = 0; i < addr->info_count; i++) {
                sock = &addr->info[(i + idx) % addr->info_count];

                if (ltgconf_global.rdma && ltgconf_global.daemon) {
                        ret = corenet_rdma_connect(sock->addr, sock->port, &sockid);
                        if (unlikely(ret))
                                continue;
                } else {
                        ret = corenet_tcp_connect(coreid, sock->addr, sock->port, &sockid);
                        if (unlikely(ret))
                                continue;

                        sockid.rdma_handler = NULL;
                }

                break;
        }

        if (i == addr->info_count) {
                ret = ENONET;
                GOTO(err_ret, ret);
        }
        
        DBUG("connect to %s/%d sd %u, addr %d:%d\n", netable_rname(&coreid->nid),
             coreid->idx, sockid.sd, sock->addr, sock->port);

        *_sockid = sockid;

        return 0;
err_ret:
        return ret;
}

int corenet_maping_register(uint64_t coremask)
{
        int ret, retry = 0;
        nid_t nid = *net_getnid();
        coreid_t coreid = {nid, 0};
        corenet_addr_t *addr;
        char buf[MAX_BUF_LEN], key[MAX_NAME_LEN];
        static int addr_count[CORE_MAX] = {0};
        
        addr = (void *)buf;
        for (int i = 0; i < CORE_MAX; i++) {
                if (!core_usedby(coremask, i))
                        continue;

                coreid.idx = i;
                ret = corenet_getaddr(&coreid, addr);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

        retry:
                if (addr->info_count != addr_count[i]) {
                        snprintf(key, MAX_NAME_LEN, "%d/%d", nid.id, i);
                        ret = etcd_create(ETCD_CORENET, key, addr, addr->len, -1);
                        if (unlikely(ret)) {
                                if (ret == ENOKEY || ret == ENOENT || ret == EEXIST) {
                                        ret = etcd_update(ETCD_CORENET, key, addr, addr->len,
                                                          NULL, -1);
                                        if (unlikely(ret))
                                                GOTO(err_ret, ret);
                                } else {
                                        USLEEP_RETRY(err_ret, ret, retry, retry, 100, (1000 * 1000));
                                }
                        }

                        DINFO("update core[%d] corenet %u -> %u\n", i, addr_count[i],
                              addr->info_count);
                        addr_count[i] = addr->info_count;
                } else {
                        DBUG("skip core[%d] %u\n", i, addr_count[i]);
                }
        }

        return 0;
err_ret:
        return ret;
}

int corenet_maping_offline(uint64_t coremask)
{
        int ret;
        nid_t nid = *net_getnid();
        coreid_t coreid = {nid, 0};
        corenet_addr_t *addr;
        char buf[MAX_BUF_LEN];
        static int addr_count[CORE_MAX] = {0};
        
        addr = (void *)buf;
        for (int i = 0; i < CORE_MAX; i++) {
                if (!core_usedby(coremask, i))
                        continue;

                coreid.idx = i;
                ret = corenet_getaddr(&coreid, addr);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                if (addr->info_count < addr_count[i]) {
                        DERROR("core[%d] corenet %u -> %u\n", i, addr_count[i],
                               addr->info_count);
                        EXIT(EAGAIN);
                }

                addr_count[i] = addr->info_count;
        }

        return 0;
err_ret:
        return ret;
}


static int __corenet_maping_connect__(const nid_t *nid, sockid_t *_sockid,
                                      uint64_t *_coremask)
{
        int ret, valuelen;
        char buf[MAX_BUF_LEN], key[MAX_NAME_LEN];
        corenet_addr_t *addr = (void *)buf;
        uint64_t coremask;
        coreid_t coreid = {*nid, 0};

        snprintf(key, MAX_NAME_LEN, "%d/coremask", nid->id);
        valuelen = sizeof(coremask);
        ret = etcd_get_bin(ETCD_CORENET, key, (void *)&coremask, &valuelen, NULL);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        int count = 0;
        for (int i = 0; i < CORE_MAX; i++) {
                if (!core_usedby(coremask, i))
                        continue;

                valuelen = MAX_NAME_LEN;
                snprintf(key, MAX_NAME_LEN, "%d/%d", nid->id, i);
                ret = etcd_get_bin(ETCD_CORENET, key, (void *)addr, &valuelen, NULL);
                if (unlikely(ret)) {
                        GOTO(err_close, ret);
                }

                coreid.idx = i;
                ret = __corenet_maping_connect_core(&coreid, addr, &_sockid[i]);
                if (unlikely(ret)) {
                        GOTO(err_close, ret);
                }

                count++;
        }

        *_coremask = coremask;

        return 0;
err_close:
        if (count) {
                DERROR("%s %d connected, restart for safe\n",
                       netable_rname(nid), count);
                EXIT(EAGAIN);
                UNIMPLEMENTED(__DUMP__);
        } else {
                DBUG("connect to %s fail\n", netable_rname(nid));
        }
err_ret:
        return ret;
}

STATIC int __corenet_maping_update(const nid_t *nid, const sockid_t *_sockid,
                                   uint64_t coremask)
{
        int ret;
        corenet_maping_t *entry;
        coreid_t coreid = {*nid, 0};

        entry = &__corenet_maping_get__()[nid->id];

        coreid_check(entry->coreid);
        
        if (ltgconf_global.rdma) {
                entry->request = corerpc_rdma_request;
                entry->connected = corenet_rdma_connected;
        } else {
                entry->request = corerpc_tcp_request;
                entry->connected = corenet_tcp_connected;
        }
        
        LTG_ASSERT(entry->connected);

        DBUG("nid %s %d, %p\n", netable_rname(nid), nid->id, entry);
        
        for (int i = 0; i < CORE_MAX; i++) {
                if (!core_usedby(coremask, i))
                        continue;
                        
                if (entry->connected(&entry->sockid[i])) {
                        DERROR("%s[%d] connected, restart for safe\n",
                               netable_rname(nid), i);
                        EXIT(EAGAIN);
                }

                coreid.idx = i;
                sockid_t sockid = _sockid[i];
                sockid.request = entry->request;
                ret = corenet_hb_add(&coreid, &sockid);
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);
        }

        memcpy(entry->sockid, _sockid, sizeof(*_sockid) * CORE_MAX);
        entry->coremask = coremask;

        __corenet_maping_resume(&entry->wait_list, nid, 0);
        
        return 0;
}

STATIC int __corenet_maping_connect(const nid_t *nid)
{
        int ret;
        sockid_t sockid[CORE_MAX];
        uint64_t coremask;

        memset(sockid, 0x00, sizeof(sockid_t) * CORE_MAX);
        ret = __corenet_maping_connect__(nid, sockid, &coremask);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = __corenet_maping_update(nid, sockid, coremask);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

err_ret:
        return ret;
}

static int S_LTG __corenet_maping_get(const coreid_t *coreid,
                                      corenet_maping_t *entry,
                                      sockid_t *_sockid)
{
        int ret;
        sockid_t *sockid;

        if (unlikely(entry->connected == NULL)) {
                ret = ENONET;
                GOTO(err_ret, ret);
        }
        
        LTG_ASSERT((int)coreid->idx < (int)CORE_MAX);
        sockid = &entry->sockid[coreid->idx];

        if (likely(entry->connected(sockid))) {
                *_sockid = *sockid;
        } else {
                ret = ENONET;
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

static void __corenet_maping_connect_task(void *arg)
{
        int ret;
        corenet_maping_t *entry = arg;
        const nid_t *nid = &entry->nid;

#if 1
        __corenet_maping_close_entry(entry, NULL);
#endif

        coreid_check(entry->coreid);
        
        DINFO("connect to %s\n", netable_rname(nid));
        ret = __corenet_maping_connect(nid);
        if (ret) {
                __corenet_maping_resume(&entry->wait_list, nid, ret);
                DWARN("connect to %s fail\n", netable_rname(nid));
        }

        entry->connecting = 0;
}

static int __corenet_maping_connect_wait_task(corenet_maping_t *entry,
                                              conn_wait_ctx_t *ctx)
{
        int ret;

        (void) entry;

        ctx->task = sche_task_get();
        
        ret = sche_yield("maping_connect", NULL, NULL);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

static int __corenet_maping_connect_wait_queue(corenet_maping_t *entry,
                                               conn_wait_ctx_t *ctx)
{
        int ret;
        const nid_t *nid = &entry->nid;
        core_t *core = core_self();

        DINFO("connect to %s\n", netable_rname(nid));
        
        ANALYSIS_BEGIN(0);

        while (!list_empty(&ctx->hook)) {
                core_worker_run(core);
        }

        ANALYSIS_QUEUE(0, IO_WARN, NULL);
 
        DINFO("connect to %s return, %d\n", netable_rname(nid), ctx->retval);
       
        ret = ctx->retval;
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

static int __corenet_maping_connect_wait(corenet_maping_t *entry)
{
        int ret;
        conn_wait_ctx_t ctx;
        const nid_t *nid = &entry->nid;

        coreid_check(entry->coreid);
        
        //if (list_empty(&entry->wait_list)) {
        if (entry->connecting == 0) {
                entry->connecting = 1;
                DBUG("connect to %s\n", netable_rname(nid));

                sche_task_new("corenet_maping", __corenet_maping_connect_task,
                              entry, -1);
        }

        if (sche_status() == SCHEDULE_STATUS_RUNNING) {
                ctx.retval = 0;
                list_add(&ctx.hook, &entry->wait_list);
                ctx.type = TYPE_TASK;
                ret = __corenet_maping_connect_wait_task(entry, &ctx);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        } else {
#if 0
                ret = ENOSYS;
                GOTO(err_ret, ret);
#endif
                
                ctx.retval = 0;
                list_add(&ctx.hook, &entry->wait_list);
                ctx.type = TYPE_QUEUE;
                ret =  __corenet_maping_connect_wait_queue(entry, &ctx);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

int S_LTG corenet_maping(void *core, const coreid_t *coreid, sockid_t *sockid)
{
        int ret;
        corenet_maping_t *entry;

        ANALYSIS_BEGIN(0);
retry:
        entry = &__corenet_maping_get_byctx(core)[coreid->nid.id];
        LTG_ASSERT(entry);

        ret = __corenet_maping_get(coreid, entry, sockid);
        if (unlikely(ret)) {
                /**
                 * 保证过程唯一性，只有一个task发起连接，其它并发task等待连接完成
                 * 发起连接的task，完成后唤醒所有等待task
                 */

                ret = __corenet_maping_connect_wait(entry);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                goto retry;
        }

        sockid->request = entry->request;

        ANALYSIS_QUEUE(0, IO_WARN, NULL);

        return 0;
err_ret:
        return ret;
}

static void __corenet_maping_close_entry(corenet_maping_t *entry,
                                         const sockid_t *_sockid)
{
        sockid_t *sockid;

        for (int i = 0; i < CORE_MAX; i++) {
                if (!core_usedby(entry->coremask, i))
                        continue;

                sockid = &entry->sockid[i];
                if (sockid->sd == -1) {
                        continue;
                }

                if (_sockid == NULL) {
                        DINFO("close all sock %s nid[%u], sockid %u\n",
                              netable_rname(&entry->nid), entry->nid.id,
                              sockid->sd);

                        __corenet_maping_close_finally__(&entry->nid, sockid);
                        sockid->sd = -1;
                        continue;
                }

                if (_sockid->sd == sockid->sd
                    && _sockid->seq == sockid->seq) {
                        DBUG("close one sock %s nid[%u], sockid %u\n",
                             netable_rname(&entry->nid), entry->nid.id,
                             sockid->sd);

                        __corenet_maping_close_finally__(&entry->nid, sockid);
                        sockid->sd = -1;
                        break;
                } else {
                        DBUG("skip close %s nid[%u], sockid %u\n",
                             netable_rname(&entry->nid), entry->nid.id,
                             sockid->sd);
                }
        }
}

static int __corenet_maping_init__(corenet_maping_t **_maping)
{
        int ret, i;
        corenet_maping_t *maping, *entry;
        nid_t nid;
        coreid_t coreid;

        ret = ltg_malloc((void **)&maping, sizeof(*maping) *  NODEID_MAX);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = core_getid(&coreid);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);
        
        for (i = 0; i < NODEID_MAX; i++) {
                nid.id = i;
                entry = &maping[i];
                INIT_LIST_HEAD(&entry->wait_list);
                entry->coremask = 0;
                entry->connecting = 0;
                entry->nid = nid;
                entry->coreid = coreid.idx;
        }

        core_tls_set(VARIABLE_MAPING, maping);
        if (_maping)
                *_maping = maping;

        return 0;
err_ret:
        return ret;
}

static int __corenet_maping_close(va_list ap)
{
        const nid_t *nid = va_arg(ap, const nid_t *);
        const sockid_t *sockid = va_arg(ap, const sockid_t *);

        va_end(ap);

        corenet_maping_t *entry, *maping;
        maping = __corenet_maping_get__();

        if (maping == NULL) {
                return 0;
        }

        entry = &maping[nid->id];

        __corenet_maping_close_entry(entry, sockid);

        return 0;
}

void corenet_maping_closeall(const nid_t *nid, const sockid_t *sockid)
{
        LTG_ASSERT(sockid);
        LTG_ASSERT(ltgconf_global.daemon);
        //LTG_ASSERT(nid->id <= UINT16_MAX);

        core_init_modules("corenet maping close", __corenet_maping_close, nid, sockid);
}

inline static void __corenet_maping_destroy(void *_core, void *var, void *_corenet_maping)
{
        core_t *core = _core;

        (void) _corenet_maping;
        (void) var;

        corenet_maping_destroy((corenet_maping_t **)&core->maping);

        return;
}

static int __corenet_maping_init(va_list ap)
{
        int ret;
        core_t *core = core_self();

        va_end(ap);

        ret = __corenet_maping_init__((corenet_maping_t **)&core->maping);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int corenet_maping_init(uint64_t mask)
{
        int ret;

        ret = core_init_modules1("corenet_maping", mask,
                                 __corenet_maping_init, NULL);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int corenet_maping_connected(const nid_t *nid, const sockid_t *sockid)
{
        corenet_maping_t *entry;

        entry = &__corenet_maping_get__()[nid->id];
        LTG_ASSERT(entry->connected);

        return entry->connected(sockid);
}

void corenet_maping_close(const nid_t *nid, const sockid_t *sockid)
{
        corenet_maping_t *entry, *maping;
        maping = __corenet_maping_get__();

        LTG_ASSERT(maping);
        entry = &maping[nid->id];

        __corenet_maping_close_entry(entry, sockid);

        return;
}
