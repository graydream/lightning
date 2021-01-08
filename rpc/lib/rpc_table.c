#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>

#define DBG_SUBSYS S_LTG_RPC

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_rpc.h"
#include "core/corenet.h"

rpc_table_t *__rpc_table__;

static int S_LTG __rpc_table_used(rpc_table_t *rpc_table, slot_t *slot)
{
        int ret;

        if (likely(rpc_table->private)) {
                return pspin_locked(&slot->used_pspin);
        } else {
                ret = ltg_spin_trylock(&slot->used_spin);
                if (unlikely(ret)) {
                        LTG_ASSERT(ret == EBUSY);
                        //LTG_ASSERT(slot->used_spin == 0);
                        return 1;
                } else {
                        ltg_spin_unlock(&slot->used_spin);
                        return 0;
                }
        }
}

static int S_LTG __rpc_table_use(rpc_table_t *rpc_table, slot_t *slot)
{
        if (likely(rpc_table->private)) {
                return pspin_trylock(&slot->used_pspin);
        } else {
                return ltg_spin_trylock(&slot->used_spin);
        }
}

static void __rpc_table_free(rpc_table_t *rpc_table, slot_t *slot)
{
        slot->post = NULL;
        slot->post_arg = NULL;
        slot->timeout = 0;
        slot->figerprint_prev = slot->msgid.figerprint;
        slot->msgid.figerprint = 0;

        if (likely(rpc_table->private)) {
                pspin_unlock(&slot->used_pspin);
        } else {
                ltg_spin_unlock(&slot->used_spin);
        }

        //__rpc_table()->cur = slot->msgid.idx;
}

static int S_LTG __rpc_table_lock(rpc_table_t *rpc_table, slot_t *slot)
{
        if (likely(rpc_table->private))
                return 0;
        else
                return ltg_spin_lock(&slot->lock);
}

static int __rpc_table_trylock(rpc_table_t *rpc_table, slot_t *slot)
{
        if (likely(rpc_table->private))
                return 0;
        else
                return ltg_spin_trylock(&slot->lock);
}

static int __rpc_table_unlock(rpc_table_t *rpc_table, slot_t *slot)
{
        if (likely(rpc_table->private))
                return 0;
        else
                return ltg_spin_unlock(&slot->lock);
}

static int __rpc_table_check(rpc_table_t *rpc_table, slot_t *slot, uint32_t now)
{
        int ret, retval = ETIMEDOUT;
        const char *conn;

        ret = __rpc_table_trylock(rpc_table, slot);
        if (unlikely(ret)) {
                if (ret == EBUSY) {
                        DINFO("%s @ %s/%u check, id (%u, %x) busy\n", slot->name,
                              _inet_ntoa(slot->sockid.addr), slot->sockid.sd, slot->msgid.idx,
                              slot->msgid.figerprint);
                        return 0;
                } else
                        GOTO(err_ret, ret);
        }

        if (slot->nid.id) {
                if (netable_connected(&slot->nid)) {
                        conn = "connected";
                } else {
                        conn = "disconnected";
                }
        } else {
                conn = "unknow";
        }

        if (slot->timeout && now > slot->timeout
            && (now - slot->timeout > (uint32_t)ltgconf_global.rpc_timeout / 2)) {
                DINFO("%s @ %s/%u(%s) check, id (%u, %x), used %u timeout %d\n", slot->name,
                      _inet_ntoa(slot->sockid.addr), slot->sockid.sd, conn, slot->msgid.idx,
                      slot->msgid.figerprint, (int)(now - slot->begin), slot->timeout);
        } else {
                DBUG("%s @ %s/%u(%s) check, id (%u, %x), used %u\n", slot->name,
                     _inet_ntoa(slot->sockid.addr), slot->sockid.sd, conn, slot->msgid.idx,
                     slot->msgid.figerprint, (int)(now - slot->begin));
        }

        if (slot->timeout && now > slot->timeout) {
                DWARN("%s @ %s/%u(%s) timeout, id (%u, %x), rpc %u "
                      "used %u timeout %d\n", slot->name,
                      _inet_ntoa(slot->sockid.addr), slot->sockid.sd,
                      conn, slot->msgid.idx,
                      slot->msgid.figerprint,
                      ltgconf_global.rpc_timeout,
                      (int)(now - slot->begin), slot->timeout);

#if 1
                slot->timeout = 0;
                slot->close(&slot->nid, &slot->sockid, NULL);
                slot->post(slot->post_arg, &retval, NULL, NULL);
                __rpc_table_free(rpc_table, slot);
#else
                sockid_t sockid = slot->sockid; 
                nid_t nid = slot->nid;
                func2_t _close = slot->close;
                
                slot->timeout = 0;
                slot->post(slot->post_arg, &retval, NULL, NULL);
                __rpc_table_free(rpc_table, slot);

                _close(&nid, &sockid, NULL);
#endif
        }

        __rpc_table_unlock(rpc_table, slot);

        return 0;
err_ret:
        return ret;
}

static void __rpc_table_scan(rpc_table_t *rpc_table)
{
        slot_t *slot;
        uint32_t i, used = 0, checked = 0;
        time_t now = gettime();
        
        ANALYSIS_BEGIN(0);
                
        for (i = 0; i < rpc_table->count; i++) {
                slot = rpc_table->slot[i];

                if (!__rpc_table_used(rpc_table, slot)) {
                        continue;
                }

                used++;

                __rpc_table_check(rpc_table, slot, now);

                checked++;
        }

        ANALYSIS_END(0, IO_WARN, NULL);

        rpc_table->last_scan = gettime();

        if (used && (rpc_table->cycle % 2 == 0)) {
                rpc_table->cycle++;
                DINFO("%s used %u/%u\n", rpc_table->name, used, checked);
        }
}

#if 0
static void __rpc_table_scan_task(void *args)
{
        rpc_table_t *rpc_table = args;

        __rpc_table_scan(rpc_table);
}
#endif

void rpc_table_scan(rpc_table_t *rpc_table, int interval, int newtask)
{
        int tmo;
        time_t now;

        (void) newtask;

        now = gettime();
        if (now < rpc_table->last_scan) {
                DERROR("update time %u --> %u\n", (int)now, (int)rpc_table->last_scan);
                rpc_table->last_scan = now;
                return;
        }

        if (now - rpc_table->last_scan > interval) {
                DBUG("scan %s now:%ld, interval %d\n", rpc_table->name, now, interval);
                tmo = (now - rpc_table->last_scan) - interval;
                if (tmo > 3 && rpc_table->last_scan) {
                        DINFO("scan %s delay %ds\n", rpc_table->name, tmo);
                }

                rpc_table->last_scan = now;
#if 0
                if (newtask) {
                        sche_task_new("rpc_table_scan", __rpc_table_scan_task, rpc_table, -1);
                } else {
                        __rpc_table_scan(rpc_table);
                }
#else
                __rpc_table_scan(rpc_table);
#endif
        }
}

static void  *__rpc_table_scan_worker(void *arg)
{
        int interval;
        rpc_table_t *rpc_table = arg;

        while (1) {
                if (rpc_table == NULL) {
                        continue;
                }

                if (ltgconf_global.daemon) {
                        sleep(2);
                } else {
                        sleep(1);
                }

                interval = _min(ltgconf_global.rpc_timeout, 1);
                rpc_table_scan(rpc_table, interval, 0);
        }

        return NULL;
}


static slot_t S_LTG *__rpc_table_getslot_sequence(rpc_table_t *rpc_table)
{
        int ret, retry = 0;
        slot_t *slot;
        uint32_t i, cur;

        cur = rpc_table->cur;
        for (i = 0; i < rpc_table->count; i++) {
                slot = rpc_table->slot[(i + cur) %  rpc_table->count];
                ret = __rpc_table_use(rpc_table, slot);
                if (unlikely(ret == EBUSY)) {
                        retry++;
                        if (retry > 1000) {
                                DWARN("retry %u\n", retry);
                        }

                        continue;
                }

                rpc_table->cur = (i + cur + 1) % rpc_table->count;

                //DINFO("cur %d\n", rpc_table->cur);

                return slot;
        }

        return NULL;
}

static void S_LTG __rpc_table_new(rpc_table_t *rpc_table, slot_t *slot)
{
        int rand, retry = 0;

        while (1) {
                rand = ++rpc_table->sequence;
                if (likely(slot->figerprint_prev != (uint32_t)rand)) {
                        slot->msgid.figerprint = rand;
                        break;
                } else {
                        LTG_ASSERT(retry < 100);
                        retry++;
                }
        }
}

int S_LTG rpc_table_getslot(rpc_table_t *rpc_table, msgid_t *msgid, const char *name)
{
        int ret;
        slot_t *slot;

        slot = __rpc_table_getslot_sequence(rpc_table);
        if (unlikely(slot == NULL)) {
                ret = ENOSPC;
                GOTO(err_ret, ret);
        }

        __rpc_table_new(rpc_table, slot);

        *msgid = slot->msgid;
        strcpy(slot->name, name);

        return 0;
err_ret:
        return ret;
}

static slot_t S_LTG *__rpc_table_lock_slot(rpc_table_t *rpc_table, const msgid_t *msgid)
{
        int ret;
        slot_t *slot;

        slot = rpc_table->slot[msgid->idx];
        if (unlikely(msgid->figerprint != slot->msgid.figerprint)) {
                DBUG("slot[%u] already closed\n", msgid->idx);
                return NULL;
        }

        ret = __rpc_table_lock(rpc_table, slot);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        if (likely(__rpc_table_used(rpc_table, slot)))
                return slot;
        else {
                DWARN("slot[%u] unused\n", msgid->idx);
                __rpc_table_unlock(rpc_table, slot);
                return NULL;
        }

        return slot;
}

int S_LTG rpc_table_setslot(rpc_table_t *rpc_table, const msgid_t *msgid, func3_t func, void *arg,
                      func2_t _close, const nid_t *nid, const sockid_t *sockid, int timeout)
{
        int ret;
        slot_t *slot;

        LTG_ASSERT(timeout <= 180);

        slot = __rpc_table_lock_slot(rpc_table, msgid);
        if (unlikely(slot == NULL)) {
                ret = ESTALE;
                GOTO(err_ret, ret);
        }

        slot->post = func;
        slot->post_arg = arg;
        slot->close = _close;
        slot->begin = gettime();
        slot->timeout = slot->begin + timeout;

        if (sockid) 
                slot->sockid = *sockid;
        else
                memset(&slot->sockid, 0x0, sizeof(*sockid));

        if (nid) {
                //LTG_ASSERT(netable_connected(nid));
                slot->nid = *nid;
        } else {
                memset(&slot->nid, 0x0, sizeof(*nid));
        }
        
        __rpc_table_unlock(rpc_table, slot);

        return 0;
err_ret:
        return ret;
}

int S_LTG rpc_table_post(rpc_table_t *rpc_table, const msgid_t *msgid, int retval,
                           ltgbuf_t *buf, uint64_t latency)
{
        int ret;
        slot_t *slot;

        (void) latency;
        
        slot = __rpc_table_lock_slot(rpc_table, msgid);
        if (unlikely(slot == NULL)) {
                ret = ESTALE;
                GOTO(err_ret, ret);
        }

        slot->post(slot->post_arg, &retval, buf, NULL);

        __rpc_table_free(rpc_table, slot);

        __rpc_table_unlock(rpc_table, slot);

        return 0;
err_ret:
        return ret;
}

int rpc_table_free(rpc_table_t *rpc_table, const msgid_t *msgid)
{
        int ret;
        slot_t *slot;

        slot = __rpc_table_lock_slot(rpc_table, msgid);
        if (slot == NULL) {
                ret = ESTALE;
                GOTO(err_ret, ret);
        }

        __rpc_table_free(rpc_table, slot);

        __rpc_table_unlock(rpc_table, slot);

        return 0;
err_ret:
        return ret;
}

static int __rpc_table_reset(rpc_table_t *rpc_table,
                             slot_t *slot, const sockid_t *sockid,
                             const nid_t *nid)
{
        int ret, retval = ECONNRESET;

        if (!__rpc_table_used(rpc_table, slot)) {
                return 0;
        }

        ret = __rpc_table_trylock(rpc_table, slot);
        if (unlikely(ret)) {
                if (ret == EBUSY) {
                        return 0;
                } else
                        GOTO(err_ret, ret);
        }

        (void) nid;
        LTG_ASSERT(sockid);
        if (slot->timeout && sockid_cmp(&slot->sockid, sockid) == 0) {
                DINFO("table %s %s @ %s(%s) reset, id (%u, %x), used %u\n",
                      rpc_table->name, slot->name,
                      _inet_ntoa(slot->sockid.addr),
                      nid ? netable_rname(nid) : "NULL", slot->msgid.idx,
                      slot->msgid.figerprint, (int)(gettime() - slot->begin));

                slot->close(&slot->nid, &slot->sockid, NULL);
                slot->post(slot->post_arg, &retval, NULL, NULL);
                __rpc_table_free(rpc_table, slot);
        }

        __rpc_table_unlock(rpc_table, slot);

        return 0;
err_ret:
        return ret;
}

void rpc_table_reset(rpc_table_t *rpc_table, const sockid_t *sockid, const nid_t *nid)
{
        uint32_t i;
        slot_t *slot;

        if (rpc_table == NULL) {
                DWARN("rpc table not inited\n");
                return;
        }

        for (i = 0; i < rpc_table->count; i++) {
                slot = rpc_table->slot[i];
                __rpc_table_reset(rpc_table, slot, sockid, nid);
        }
}

static int __rpc_table_create(const char *name, int count, int tabid,
                              int private, rpc_table_t **_rpc_table)
{
        int ret, i;
        slot_t *slot;
        rpc_table_t *rpc_table;

        uint32_t size = sizeof(rpc_table_t) + sizeof(slot_t *) * count;
        if (private) {
                ret = slab_static_alloc1((void **)&rpc_table, size);
        } else {
                ret = ltg_malloc((void **)&rpc_table, size);
        }
        if (unlikely(ret))
                GOTO(err_ret, ret);

        for (i = 0; i < count; i++) {
                if (private) {
                        ret = slab_static_alloc1((void **)&slot, sizeof(*slot));
                } else {
                        ret = ltg_malloc((void **)&slot, sizeof(*slot));
                }
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);
                
                ret = ltg_spin_init(&slot->lock);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                if (private) {
                        ret = pspin_init(&slot->used_pspin);
                } else {
                        ret = ltg_spin_init(&slot->used_spin);
                }
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                slot->msgid.idx = i;
                slot->msgid.tabid = tabid;
                slot->msgid.figerprint = 0;
                slot->figerprint_prev = 0;
                slot->timeout = 0;
                slot->name[0] = '\0';
                slot->post_arg = NULL;
                slot->post = NULL;

                rpc_table->slot[i] = slot;
        }

        strcpy(rpc_table->name, name);
        rpc_table->sequence = _random();
        rpc_table->count = count;
        rpc_table->tabid = tabid;
        rpc_table->last_scan = 0;
        rpc_table->private = private;
        *_rpc_table = rpc_table;

        return 0;
err_ret:
        return ret;
}

int rpc_table_init(const char *name, rpc_table_t **rpc_table, int private)
{
        int ret, count;
        rpc_table_t *tmp;

        count = RPC_TABLE_MAX;
        ret = __rpc_table_create(name, count, 0, private, &tmp);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (!private) {
                pthread_t th;
                pthread_attr_t ta;

                (void) pthread_attr_init(&ta);
                (void) pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);

                ret = pthread_create(&th, &ta, __rpc_table_scan_worker, tmp);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }

        *rpc_table = tmp;

        return 0;
err_ret:
        return ret;
}


void rpc_table_destroy(rpc_table_t **_rpc_table)
{
        slot_t *slot;
        rpc_table_t *rpc_table = *_rpc_table;
        int retval = ECONNRESET;

        for (int i = 0; i < (int)rpc_table->count; i++) {
                slot = rpc_table->slot[i];

                if (!__rpc_table_used(rpc_table, slot)) {
                        continue;
                }

                slot->post(slot->post_arg, &retval, NULL, NULL);
                __rpc_table_free(rpc_table, slot);
                UNIMPLEMENTED(__DUMP__);
        }

        if (rpc_table->private) {
                slab_static_free1((void **)&rpc_table);
        } else {
                ltg_free((void **)&rpc_table);
        }
}
