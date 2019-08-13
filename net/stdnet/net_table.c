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
#include "ltg_rpc.h"
#include "ltg_core.h"

#define INFO_QUEUE_LEN (2 * 1024 * 1024)
#define LNET_NAME_UNKNOWN   "unknown"

static uint32_t __ltime_seq__ = 1;

#define LTIME_UPDATE(__lname__, __ltime__)                               \
        do {                                                            \
                time_t now;                                             \
                int __retry__ = 0;                                            \
                                                                        \
                while (1) {                                             \
                        now = ++__ltime_seq__;                          \
                        if ((__ltime__)->prev == now || now == 0) {     \
                                DWARN("conn %s too fast, %u %u retry %u\n", __lname__, (int)(__ltime__)->prev, (int)now, __retry__); \
                                if (__retry__ > 10) {                   \
                                        EXIT(EAGAIN);                   \
                                }                                       \
                                __retry__++;                            \
                                continue;                               \
                        }                                               \
                        break;                                          \
                }                                                       \
                                                                        \
                (__ltime__)->prev = now;                                      \
                (__ltime__)->now = now;                                       \
        } while (0)

#define LTIME_INIT(__ltime__)              \
        do {                                    \
                (__ltime__)->now = 0;                \
                (__ltime__)->prev = 0;               \
        } while (0)

#define LTIME_DROP(__ltime__)              \
        do {                                    \
                (__ltime__)->now = 0;          \
        } while (0)


typedef struct {
        int64_t load;
        nid_t nid;
        diskid_t diskid;
} section_t;

typedef ltg_net_conn_t entry_t;

typedef struct {
        entry_t *ent;
        ltg_rwlock_t rwlock;
} net_table_t;

static net_table_t **__net_table__ = NULL;

int netable_rdlock(const nid_t *nid)
{
        LTG_ASSERT(nid->id);
        return ltg_rwlock_rdlock(&__net_table__[nid->id]->rwlock);
}

int netable_wrlock(const nid_t *nid)
{
        LTG_ASSERT(nid->id);
        return ltg_rwlock_wrlock(&__net_table__[nid->id]->rwlock);
}

void netable_unlock(const nid_t *nid)
{
        LTG_ASSERT(nid->id);
        ltg_rwlock_unlock(&__net_table__[nid->id]->rwlock);
}

static void __netable_update_ltime(entry_t *ent)
{
        LTIME_UPDATE(ent->lname, &ent->ltime);
}

static int __entry_load(entry_t *ent, const ltg_net_info_t *info,
                        const net_handle_t *sock)
{
        LTG_ASSERT(ent->ltime.now == 0);
        ent->sock = *sock;
        LTG_ASSERT(sock->u.sd.type == SOCKID_NORMAL);
        ent->status = NETABLE_CONN;
        ent->timeout = 1000 * 1000 * ltgconf.hb_timeout;
        sprintf(ent->lname, "%s", info->name);
        __netable_update_ltime(ent);

        DBUG("info.name %s \n", ent->lname);

        return 0;
}

static int __entry_create(entry_t **_ent, const nid_t *nid)
{
        int ret;
        entry_t *ent;

        ret = slab_static_alloc1((void **)&ent, sizeof(*ent));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        memset(ent, 0x0, sizeof(*ent));

        ent->nh.type = NET_HANDLE_PERSISTENT;
        ent->nh.u.nid = *nid;
        ent->status = NETABLE_NULL;
        ent->update = gettime();
        ent->last_retry = 0;
        LTIME_INIT(&ent->ltime);

        *_ent = ent;

        return 0;
err_ret:
        return ret;
}

inline static ltg_net_conn_t *__netable_nidfind(const nid_t *nid)
{
        LTG_ASSERT(nid->id < NODEID_MAX);
        return __net_table__[nid->id]->ent;
}

static int __iterate_handler(void *arg_null, void *net)
{
        int ret;
        (void) arg_null;
        entry_t *ent = (entry_t *) net;
        nid_t nid = ent->nh.u.nid;

        ret = netable_rdlock(&nid);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        DINFO("%s, load: %llu, status %u\n", ent->lname,
              (LLU)ent->load, ent->status);

        netable_unlock(&nid);

        return 0;
}

typedef struct {
	nid_t nid;
	time_t ltime;
} arg_t;

static void __netable_close(void *_arg)
{
        arg_t *arg = _arg;

        netable_close(&arg->nid, "close by sdevent", &arg->ltime);

        ltg_free((void **)&_arg);
}

static int __netable_connect__(entry_t *ent, const net_handle_t *sock,
                               const ltg_net_info_t *info, int flag)
{
        int ret;
        arg_t *arg;

        LTG_ASSERT(sock->type == NET_HANDLE_TRANSIENT);
        LTG_ASSERT(sock->u.sd.type == SOCKID_NORMAL);
        
        if (ent->status == NETABLE_CONN) {
                if (flag) {
                        ret = EEXIST;
                        GOTO(err_ret, ret);
                } else {
                        DINFO("%s already connected\n", ent->lname);
                        goto out;
                }
        }

        ret = __entry_load(ent, info, sock);
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        ret = ltg_malloc((void **)&arg, sizeof(*arg));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        arg->nid = ent->nh.u.nid;
	arg->ltime = ent->ltime.now;
        ret = sdevent_add(sock, &ent->nh.u.nid, LTG_EPOLL_EVENTS, arg, __netable_close);
        if (unlikely(ret))
                GOTO(err_free, ret); /*XXX:clean*/
        
        DINFO("add heartbeat to %s\n", ent->lname);
        ret = heartbeat_add(&ent->sock.u.sd, &ent->nh.u.nid,
                            ent->timeout, ent->ltime.now);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        ent->last_retry = 0;

out:
        return 0;
err_free:
        ltg_free((void **)&arg);
err_ret:
        return ret;
}

static int __network_connect2(entry_t *ent, const ltg_net_info_t *info)
{
        int ret;
        net_handle_t sock;

        nid_t nid = info->id;
        ret = netable_wrlock(&nid);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (ent->status == NETABLE_CONN) {
                DINFO("connect to %s sockid %s/%d time %u, exist\n",
                      ent->lname, _inet_ntoa(ent->sock.u.sd.addr),
                      ent->sock.u.sd.sd, (int)ent->ltime.now);
                goto out;
        }
        
        ANALYSIS_BEGIN(0);
        ret = net_connect(&sock, info, 2);
        if (unlikely(ret)) {
                DWARN("connect to %s fail ret %u %s\n", info->name, ret, strerror(ret));
                if (ret == EBADF)
                        ret = ETIMEDOUT;
                GOTO(err_lock, ret);
        }

        ANALYSIS_END(0, IO_WARN, NULL);
        
        ret = __netable_connect__(ent, &sock, info, 1);
        if (unlikely(ret)) {
                GOTO(err_lock, ret);
        }

        DINFO("connect to %s sockid %s/%d\n",
              ent->lname, _inet_ntoa(ent->sock.u.sd.addr), ent->sock.u.sd.sd);

out:
        netable_unlock(&nid);

        return 0;
err_lock:
        LTIME_DROP(&ent->ltime);
        ent->status = NETABLE_DEAD;
        ent->last_retry = gettime();
        netable_unlock(&nid);
err_ret:
        return ret;
}

int netable_init(int daemon)
{
        int ret, i;
        net_table_t **array, *netable;

        (void) daemon;

        ret = slab_static_alloc1((void**)&array, sizeof(*array) * NODEID_MAX);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        for (i = 0; i < NODEID_MAX; i++) {
                ret = slab_static_alloc1((void**)&netable, sizeof(*netable));
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);

                array[i] = netable;
                
                netable->ent = NULL;
                ret = ltg_rwlock_init(&netable->rwlock, "net_table.rwlock");
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }

        __net_table__ = array;

        return 0;
err_ret:
        return ret;
}

static int __netable_new(const nid_t *nid, entry_t **_ent)
{
        int ret;
        entry_t *ent = NULL;

        ret = netable_wrlock(nid);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (__netable_nidfind(nid)) {
                ret = EEXIST;
                GOTO(err_lock, ret);
        }

        ret = __entry_create(&ent, nid);
        if (unlikely(ret)) {
                GOTO(err_lock, ret);
        }

        __net_table__[nid->id]->ent = ent;

        netable_unlock(nid);
        
        *_ent = ent;

        return 0;
err_lock:
        netable_unlock(nid);
err_ret:
        return ret;
}

int netable_connect_info(net_handle_t *nh, const ltg_net_info_t *info, int force)
{
        int ret;
        entry_t *ent;

        (void) force;

        LTG_ASSERT(!net_isnull(&info->id));

        main_loop_hold();

retry:
        ent = __netable_nidfind(&info->id);
        if (ent == NULL) {
                ret = __netable_new(&info->id, &ent);
                if (unlikely(ret)) {
                        if (ret == EEXIST) {
                                DWARN("connect exist\n");
                                goto retry;
                        } else
                                GOTO(err_ret, ret);
                }
        }

        if (ent->status != NETABLE_CONN) {
                ret = __network_connect2(ent, info);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }
        }

        id2nh(nh, &info->id);

        return 0;
err_ret:
        return ret;
}


static void __netable_close1(entry_t *ent)
{
        LTG_ASSERT(ent->status == NETABLE_CONN);

        LTIME_DROP(&ent->ltime);
        ent->status = NETABLE_DEAD;

        DBUG("net %s closed\n", ent->lname);
}

void netable_close(const nid_t *nid, const char *why, const time_t *ltime)
{
        int ret;
        entry_t *ent = NULL;
        net_handle_t sock;

        ent = __netable_nidfind(nid);
        if (ent == NULL) {
                return;
        }

        ret = netable_wrlock(nid);
        if (unlikely(ret)) {
                EXIT(EAGAIN);
        }

        DBUG("close %s by %s %d -> %d \n", ent->lname, why,
             ent->ltime.now, ltime ? (int)*ltime : -1);
        if (ent->status != NETABLE_CONN || (ltime && ent->ltime.now != *ltime)) {
                ret = EBUSY;
		goto err_lock;
        }

        DINFO("close %s by '%s', ltime %p\n", ent->lname, why, ltime);

        sock = ent->sock;
        __netable_close1(ent);

        netable_unlock(nid);

        sdevent_close_force(&sock);

        return;
err_lock:
        netable_unlock(nid);
///err_ret:
        return;
}

const char *__netable_rname(const nid_t *nid)
{
        int ret;
        char name[MAX_NAME_LEN];
        static __thread char buf[MAX_MSG_SIZE];

        if (__net_table__ == NULL || net_isnull(nid)) {
                snprintf(buf, MAX_NAME_LEN, ""LNET_NAME_UNKNOWN"("NID_FORMAT")",
                         NID_ARG(nid));
        } else {
                ret = maping_nid2host(nid, name);
                if (unlikely(ret)) {
                        snprintf(buf, MAX_NAME_LEN, ""LNET_NAME_UNKNOWN"("NID_FORMAT")",
                                 NID_ARG(nid));
                } else {
                        sprintf(buf, "%s", name);
                }
        }

        return buf;
}

const char *netable_rname_nid(const nid_t *nid)
{
        entry_t *ent;

        if (__net_table__ == NULL || net_isnull(nid)) {
                return __netable_rname(nid);
        } else {
                ent = __netable_nidfind(nid);

                if (ent && strlen(ent->lname)) {
                        return ent->lname;
                } else {
                        return __netable_rname(nid);
                }
        }
}

void netable_load_update(const nid_t *nid, uint64_t load)
{
        //int ret;
        entry_t *ent;

        ANALYSIS_BEGIN(0);

        if (unlikely(net_isnull(nid))) {
                return;
        }

        ent = __netable_nidfind(nid);
        if (unlikely(ent == NULL)) {
                return;
        }

        DBUG("update %s latency %llu\n", ent->lname, (LLU)load);
        ent->load = load;

        ANALYSIS_END(0, IO_WARN, NULL);

        return;
}

void netable_iterate(void)
{
        int i;
        net_table_t *net_table;

        for (i = 0; i < NODEID_MAX; i++) {
                net_table = __net_table__[i];
                if (net_table->ent) {
                        __iterate_handler(NULL, net_table->ent);
                }
        }
}

int netable_nodeid(const nid_t *nid, char *nodeid)
{
        int ret;
        entry_t *ent;

        ent = __netable_nidfind(nid);
        if (ent == NULL) {
                ret = ENONET;
                DBUG("nid "NID_FORMAT" no found\n", NID_ARG(nid));
                GOTO(err_ret, ret);
        }

        ret = netable_rdlock(nid);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if ((int)ent->ltime.now == 0) {
                ret = ENONET;
                DBUG("nid "NID_FORMAT" not connected\n", NID_ARG(nid));
                GOTO(err_lock, ret);
        }

        if (ent->status == NETABLE_CONN) {
                strcpy(nodeid, ent->nodeid);
        } else {
                ret = ENONET;
                GOTO(err_lock, ret);
        }

        netable_unlock(nid);

        return 0;
err_lock:
        netable_unlock(nid);
err_ret:
        return ret;
}

time_t IO_FUNC netable_conn_time(const nid_t *nid)
{
        entry_t *ent;

        ent = __netable_nidfind(nid);
        if (unlikely(ent == NULL))
                return 0;

        return ent->ltime.now;
}

int netable_connected(const nid_t *nid)
{
        entry_t *ent;

        ent = __netable_nidfind(nid);
        if (ent == NULL)
                return 0;

        if (ent->status == NETABLE_CONN && ent->ltime.now != 0 ) {
                return 1;
        } else {
                return 0;
        }
}

#if 0
#define REPLICA_MAX 8

static int __netable_load_cmp(const void *arg1, const void *arg2)
{
        const section_t *sec1 = arg1, *sec2 = arg2;
        return sec1->load - sec2->load;
}

void netable_sort(diskid_t *nids, int count)
{
        int ret, i;
        ltg_net_conn_t *net;
        section_t section[REPLICA_MAX], *sec;
        char buf[MAX_NAME_LEN];

        LTG_ASSERT(count <= REPLICA_MAX);
        LTG_ASSERT(count * sizeof(nid_t) < MAX_BUF_LEN);

        memcpy(buf, nids, count * sizeof(nid_t));

        for (i = 0; i < count; i++) {
                sec = &section[i];
                sec->diskid = nids[i];
                ret = d2n_nid(&sec->diskid, &sec->nid);
                if (unlikely(ret)) {
                        UNIMPLEMENTED(__DUMP__);
                }

                if (net_islocal(&sec->nid)) {
                        sec->load = core_latency_get();
                } else {
                        net = __netable_nidfind(&sec->nid);
                        if (unlikely(net == NULL || net->status != NETABLE_CONN)) {
                                DINFO("%s not online, no balance\n",
                                      netable_rname_nid(&sec->nid));
                                return;
                        }

                        DBUG("%s latency %llu\n", netable_rname_nid(&sec->nid),
                             (LLU)net->load);

                        if (net->load < 0.1)
                                sec->load = 0;
                        else
                                sec->load = net->load;
                }
        }

        qsort(section, count, sizeof(section_t), __netable_load_cmp);
        for (i = 0; i < count; i++) {
                sec = &section[i];
                nids[i] = sec->diskid;

                DBUG("node[%u] %s latency %llu\n", i, netable_rname_nid(&sec->nid),
                     (LLU)section[i].load);
        }
}
#endif

int netable_update_retry(const nid_t *nid)
{
        int ret;
        entry_t *ent = NULL;

retry:
        ent = __netable_nidfind(nid);
        if (ent == NULL) {
                DBUG("add null ent for %u\n", nid->id);
                ret = __netable_new(nid, &ent);
                if (unlikely(ret)) {
                        if (ret == EEXIST) {
                                goto retry;
                        } else
                                GOTO(err_ret, ret);
                }
        }

        ent->last_retry = gettime();

        return 0;
err_ret:
        return ret;
}

int netable_connectable(const nid_t *nid, int force)
{
        int tmo, wait;
        entry_t *ent;

        if (force)
                return 1;

        wait = ltgconf.lease_timeout / 2;
        ent = __netable_nidfind(nid);
        if (ent == NULL)
                return 1;
        else {
                tmo = gettime() - ent->last_retry;
                if (tmo < wait) {
                        DBUG("conn %s will retry after %d sec\n",
                             netable_rname_nid(nid), wait - tmo);
                        return 0;
                } else {
                        return 1;
                }
        }
}

int netable_getsock(const nid_t *nid, sockid_t *sockid)
{
        int ret;
        entry_t *ent;

        LTG_ASSERT(nid->id);
        
        ent = __netable_nidfind(nid);
        if (ent == NULL) {
                ret = ENONET;
                DWARN("nid "NID_FORMAT" not online\n", NID_ARG(nid));
                GOTO(err_ret, ret);
        }

        ret = netable_rdlock(nid);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (ent->status == NETABLE_CONN) {
                *sockid = ent->sock.u.sd;
                LTG_ASSERT(sockid->type == SOCKID_NORMAL);
        } else {
                ret = ENONET;
                DBUG("nid "NID_FORMAT"\n", NID_ARG(&ent->nh.u.nid));
                GOTO(err_lock, ret);
        }

        netable_unlock(nid);

        ret = sdevent_check(sockid);
        if (unlikely(ret)) {
                DWARN("%s lost\n", network_rname(nid));
                netable_close(nid, "lost socket", NULL);
                GOTO(err_ret, ret);
        }
        
        return 0;
err_lock:
        netable_unlock(nid);
err_ret:
        return ret;
}

STATIC int __netable_accept(entry_t *ent, const net_handle_t *sock,
                            const ltg_net_info_t *info)
{
        int ret;
        nid_t nid = info->id;

        ret = netable_wrlock(&nid);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (ent->status == NETABLE_CONN) {
                if (net_getnid()->id > info->id.id) {
                        ret = ECONNRESET;
                        GOTO(err_lock, ret);
                } else if (net_getnid()->id == info->id.id) {
                        ret = sdevent_add(sock, &info->id, LTG_EPOLL_EVENTS, NULL, NULL);
                        if (unlikely(ret))
                                GOTO(err_lock, ret);

                        DINFO("local conn %s\n", info->name);
                        goto out;
                } else {
                        DINFO("dup conn, close exist conn of %s\n",
                              info->name);

                        __netable_close1(ent);
                        sdevent_close_force(&ent->sock);
                        rpc_table_reset(__rpc_table__, &ent->sock.u.sd, &info->id);
                }
        }

        ret = __netable_connect__(ent, sock, info, 0);
        if (unlikely(ret))
                GOTO(err_lock, ret);

out:
        netable_unlock(&nid);

        return 0;
err_lock:
        netable_unlock(&nid);
err_ret:
        return ret;
}

int netable_accept(const ltg_net_info_t *info, const net_handle_t *sock)
{
        int ret;
        entry_t *ent;

        LTG_ASSERT(sock->u.sd.type == SOCKID_NORMAL);
        
        DINFO("accept %s sd %d\n", info->name, sock->u.sd.sd);
        
        LTG_ASSERT(!net_isnull(&info->id));

retry:
        ent = __netable_nidfind(&info->id);
        if (ent == NULL) {
                ret = __netable_new(&info->id, &ent);
                if (unlikely(ret)) {
                        if (ret == EEXIST) {
                                DINFO("accept %s sd %d, retry\n", info->name,
                                      sock->u.sd.sd);
                                goto retry;
                        } else
                                GOTO(err_ret, ret);
                }
        }

        ret = __netable_accept(ent, sock, info);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int netable_rname1(const nid_t *nid, char *name)
{
        int ret;
        entry_t *ent;

        if (__net_table__ == NULL || net_isnull(nid)) {
                ret = ENONET;
                GOTO(err_ret, ret);
        }
        
        if (net_islocal(nid)) {
                strcpy(name, ng.name);
        } else {
                ent = __netable_nidfind(nid);
                
                if (ent && strlen(ent->lname)) {
                        strcpy(name, ent->lname);
                } else {
                        ret = ENONET;
                        GOTO(err_ret, ret);
                }
        }

        DBUG("%u %s\n", nid->id, name);
        
        return 0;
err_ret:
        return ret;
}

void netable_update(const nid_t *nid)
{
        entry_t *ent;

        ent = __netable_nidfind(nid);
        if (ent == NULL)
                return;

        if (ent->status == NETABLE_CONN) {
                ent->update = gettime();
                DBUG("update %s\n", ent->lname);
        }
}

time_t netable_last_update(const nid_t *nid)
{
        entry_t *ent;

        ent = __netable_nidfind(nid);
        if (ent == NULL)
                return 0;

        return ent->update;
}
