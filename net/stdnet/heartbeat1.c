#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_net.h"
#include "ltg_utils.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

extern int ltg_nofile_max;

typedef struct {
        uint64_t timeout;
        uint64_t reply;
        uint64_t sent;
        sockid_t sockid;
        int retry;
        int refcount;
        void *ctx;
        int (*connected)(void *);
        int (*send)(void *, uint64_t);
        int (*close)(void *);
        int (*free)(void *);
        char name[MAX_NAME_LEN];
} entry_t;

int heartbeat_init()
{
        return 0;
}


static int __heartbeat_check(entry_t *ent)
{
        int ret, lost;
        uint64_t sent, reply;

        if (!ent->connected(ent->ctx)) {
                DWARN("%s already closed\n", ent->name);
                ret = ENONET;
                GOTO(err_ret, ret);
        }

        sent = ent->sent;
        reply = ent->reply;

        lost = sent - reply;
        if (lost && lost <= ent->retry) {
                DINFO("heartbeat %s lost ack %u\n", ent->name, lost);
        } else if (lost > ent->retry) {
                DWARN("heartbeat %s fail, lost ack %u\n", ent->name, lost);
                ret = ETIME;
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

static void __heartbeat_close(entry_t *ent)
{
        ent->close(ent->ctx);

        while (ent->refcount) {
                DWARN("wait for free %d\n", ent->refcount);
                sche_task_sleep("close sleep", 1000);
        }

        ent->free(ent->ctx);
        ltg_free((void **)&ent);
        
        return;
}

static void __heartbeat_task(void *_ent)
{
        int ret;
        uint64_t sent;
        entry_t *ent = _ent;

        ent->sent++;
        sent = ent->sent;

        DBUG("heartbeat to %s/%s send %llu reply %llu\n", ent->name,
              _inet_ntoa(ent->sockid.addr), (LLU)ent->sent, (LLU)ent->reply);
        
        ret = ent->send(ent->ctx, sent);
        if (unlikely(ret)) {
                DWARN("heartbeat %s/%s fail ret:%d, seq %ju\n", ent->name,
                      _inet_ntoa(ent->sockid.addr), ret, sent);
        } else {
                ent->reply = (ent->reply > sent) ? ent->reply : sent;
                DBUG("heartbeat to %s/%s send %llu reply %llu\n", ent->name,
                     _inet_ntoa(ent->sockid.addr), (LLU)ent->sent, (LLU)ent->reply);
        }
        
        ent->refcount--;
}

static void __heartbeat_task_new(entry_t *ent)
{
        ent->refcount++;
        sche_task_new("corenet_tcp_close", __heartbeat_task, ent, -1);
}

static void __heartbeat_loop(void *_ent)
{
        int ret;
        entry_t *ent = _ent;

        sche_task_sleep("heartbeat sleep", ent->timeout);

        ret = __heartbeat_check(ent);
        if (ret) {
                __heartbeat_close(ent);
                return;
        }

        __heartbeat_task_new(ent);
        sche_task_new("corenet_tcp_close", __heartbeat_loop, ent, -1);
}

int heartbeat_add1(const sockid_t *sockid, const char *name, void *ctx,
                   int (*connected)(void *), int (*send)(void *, uint64_t),
                   int (*close)(void *), int (*free)(void *),
                   suseconds_t timeout, int retry)
{
        int ret;
        entry_t *ent;

        LTG_ASSERT(sockid->addr);

        ret = ltg_malloc((void **)&ent, sizeof(*ent));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ent->ctx = ctx;
        ent->send = send;
        ent->connected = connected;
        ent->close = close;
        ent->free = free;
        ent->sockid = *sockid;
        ent->timeout = timeout;
        ent->retry = retry;
        ent->refcount = 0;
        ent->sent = 0;
        ent->reply = 0;
        strcpy(ent->name, name);

        DINFO("add heartbeat %s/%s, timeout %f\n", ent->name,
              _inet_ntoa(ent->sockid.addr), (float)timeout / 1000 / 1000);

        if (sche_self()) {
                sche_task_new("heartbeat", __heartbeat_loop, ent, -1);
        } else {
                ret = main_loop_request(__heartbeat_loop, ent, "heartbeat");
                LTG_ASSERT(ret == 0);
        }

        return 0;
err_ret:
        return ret;
}
