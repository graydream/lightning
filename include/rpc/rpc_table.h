#ifndef __RPC_TABLE_H__
#define __RPC_TABLE_H__

#include "ltg_utils.h"

typedef struct {
        ltg_spinlock_t lock;
        union {
                ltg_spinlock_t used_spin;
                pspin_t used_pspin;
        };
        msgid_t msgid;
        sockid_t sockid;
        nid_t nid;
        uint32_t timeout;
        uint32_t begin;
        uint32_t figerprint_prev;
        char name[MAX_NAME_LEN];
        func3_t post;
        func2_t close;
        func_t free;
        void *post_arg;
        void *free_arg;
} slot_t;

typedef struct {
        char name[MAX_NAME_LEN];
        int private;
        uint32_t count;
        uint32_t cur;
        int cycle;
        int tabid;
        time_t last_scan;
        uint32_t sequence;
        slot_t *slot[0];
} rpc_table_t;

#define RPC_TABLE_MAX 8192

extern rpc_table_t *__rpc_table__;

#define RPC_TABLE_POST_FREE 0

int rpc_table_init(const char *name, rpc_table_t **rpc_table, int private);
void rpc_table_destroy(rpc_table_t **_rpc_table);

void rpc_table_scan(rpc_table_t *rpc_table, int interval, int newtask);

int rpc_table_getslot(rpc_table_t *rpc_table, msgid_t *msgid, const char *name);
int rpc_table_setslot(rpc_table_t *rpc_table, const msgid_t *msgid, func3_t func, void *arg,
                      func2_t _close, const nid_t *nid, const sockid_t *sockid, int timeout);

int rpc_table_post(rpc_table_t *rpc_table, const msgid_t *msgid, int retval, ltgbuf_t *buf, uint64_t latency);
int rpc_table_free(rpc_table_t *rpc_table, const msgid_t *msgid);
void rpc_table_reset(rpc_table_t *rpc_table, const sockid_t *sockid, const nid_t *nid);
int rpc_table_setfree(rpc_table_t *rpc_table, const msgid_t *msgid,
                      func_t func, void *arg);

#endif
