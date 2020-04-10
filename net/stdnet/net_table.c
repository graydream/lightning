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

#define LTIME_UPDATE(__lname__, __ltime__)                              \
        do {                                                            \
                time_t now;                                             \
                int __retry__ = 0;                                      \
                                                                        \
                while (1) {                                             \
                        now = ++__ltime_seq__;                          \
                        if ((__ltime__)->prev == now || now == 0) {     \
                                DWARN("conn %s too fast, %u %u retry %u\n", \
                                      __lname__, (int)(__ltime__)->prev, \
                                      (int)now, __retry__);             \
                                if (__retry__ > 10) {                   \
                                        EXIT(EAGAIN);                   \
                                }                                       \
                                __retry__++;                            \
                                continue;                               \
                        }                                               \
                        break;                                          \
                }                                                       \
                                                                        \
                (__ltime__)->prev = now;                                \
                (__ltime__)->now = now;                                 \
        } while (0)

#define LTIME_INIT(__ltime__)                   \
        do {                                    \
                (__ltime__)->now = 0;           \
                (__ltime__)->prev = 0;          \
        } while (0)

#define LTIME_DROP(__ltime__)                  \
        do {                                   \
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

static int __entry_load(entry_t *ent, const ltg_net_info_t *info,
                        const net_handle_t *sock)
{
        LTG_ASSERT(ent->ltime.now == 0);
        ent->sock = *sock;
        LTG_ASSERT(sock->u.sd.type == SOCKID_NORMAL);
        ent->status = NETABLE_CONN;
        ent->timeout = 1000 * 1000 * ltgconf_global.hb_timeout;
        sprintf(ent->lname, "%s", info->name);
        LTIME_UPDATE(ent->lname, &ent->ltime);

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

static int __netable_connect__(entry_t *ent, const net_handle_t *sock,
                               const ltg_net_info_t *info)
{
        int ret;

        LTG_ASSERT(sock->type == NET_HANDLE_TRANSIENT);
        LTG_ASSERT(sock->u.sd.type == SOCKID_NORMAL);
        
        ret = __entry_load(ent, info, sock);
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        ret = sdevent_add(sock, &ent->nh.u.nid, LTG_EPOLL_EVENTS);
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        ret = heartbeat_add(&ent->sock.u.sd, &ent->nh.u.nid,
                            ent->timeout, ent->ltime.now);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        return 0;
err_ret:
        return ret;
}

static int __network_connect(entry_t *ent, const ltg_net_info_t *info)
{
        int ret;
        net_handle_t sock;

        ANALYSIS_BEGIN(0);
        
        nid_t nid = info->id;
        ret = netable_wrlock(&nid);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (ent->status == NETABLE_CONN) {
                DBUG("connect to %s sockid %s/%d time %u, exist\n",
                      ent->lname, _inet_ntoa(ent->sock.u.sd.addr),
                     ent->sock.u.sd.sd, (int)ent->ltime.now);
                goto out;
        }
        
        ret = net_connect(&sock, info, 1);
        if (unlikely(ret)) {
                DBUG("connect to %s fail ret %u %s\n", info->name,
                     ret, strerror(ret));
                if (ret == EBADF)
                        ret = ETIMEDOUT;
                GOTO(err_lock, ret);
        }

        ret = __netable_connect__(ent, &sock, info);
        if (unlikely(ret)) {
                GOTO(err_lock, ret);
        }

        DINFO("connect to %s sockid %s/%d\n", ent->lname,
              _inet_ntoa(ent->sock.u.sd.addr), ent->sock.u.sd.sd);

out:
        ANALYSIS_END(0, IO_INFO, NULL);

        netable_unlock(&nid);

        return 0;
err_lock:
        ANALYSIS_END(0, IO_INFO, NULL);
        LTIME_DROP(&ent->ltime);
        ent->status = NETABLE_DEAD;
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

int netable_connect(net_handle_t *nh, const ltg_net_info_t *info)
{
        int ret;
        entry_t *ent;

        LTG_ASSERT(!net_isnull(&info->id));

        main_loop_hold();

#if 0
        if (netable_connected(&info->id)) {
                DBUG("%s already connected\n", netable_rname(&info->id));
                return 0;
        }
#endif

retry:
        ent = __netable_nidfind(&info->id);
        if (ent == NULL) {
                ret = __netable_new(&info->id, &ent);
                if (unlikely(ret)) {
                        if (ret == EEXIST) {
                                DBUG("connect exist\n");
                                goto retry;
                        } else
                                GOTO(err_ret, ret);
                }
        }

        if (ent->status != NETABLE_CONN) {
                ret = __network_connect(ent, info);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }
        }

        id2nh(nh, &info->id);
        
        return 0;
err_ret:
        return ret;
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

        LTG_ASSERT(ent->status == NETABLE_CONN);

        LTIME_DROP(&ent->ltime);
        ent->status = NETABLE_DEAD;

        netable_unlock(nid);

        sdevent_close(&sock);

        return;
err_lock:
        netable_unlock(nid);
///err_ret:
        return;
}

const char *__netable_rname(const nid_t *nid)
{
        int ret;
        char tmp[MAX_BUF_LEN];
        static __thread char buf[MAX_MSG_SIZE];
        ltg_net_info_t *info;

        if (__net_table__ == NULL || net_isnull(nid)) {
                snprintf(buf, MAX_NAME_LEN, ""LNET_NAME_UNKNOWN"("NID_FORMAT")",
                         NID_ARG(nid));

                goto out;
        }

        info = (void *)tmp;
        ret = conn_getinfo(nid, info);
        if (unlikely(ret)) {
                snprintf(buf, MAX_NAME_LEN, ""LNET_NAME_UNKNOWN"("NID_FORMAT")",
                         NID_ARG(nid));
                goto out;
        }

        strcpy(buf, info->name);

out:
        return buf;
}

const char *netable_rname(const nid_t *nid)
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
        if (ent == NULL) {
                DBUG("%s not found\n", netable_rname(nid));
                return 0;
        }
        
        if (ent->status != NETABLE_CONN || ent->ltime.now == 0 ) {
                DBUG("%s status fail\n", netable_rname(nid));
                return 0;
        }

#if 0
        if (unlikely(sdevent_connected(&ent->sock.u.sd))) {
                DBUG("%s lost\n", netable_rname(nid));
                netable_close(nid, "lost socket", NULL);
                return 0;
        }
#endif

        DBUG("%s online\n", netable_rname(nid));
        
        return 1;
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

        if (unlikely(!sdevent_connected(sockid))) {
                DWARN("%s lost\n", netable_rname(nid));
                netable_close(nid, "lost socket", NULL);
                GOTO(err_ret, ret);
        }
        
        return 0;
err_lock:
        netable_unlock(nid);
err_ret:
        return ret;
}
