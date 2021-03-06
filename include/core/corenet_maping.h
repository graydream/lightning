#ifndef __CORENET_MAPING_H__
#define __CORENET_MAPING_H__

#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

#define CORENET_DEV_MAX 10

typedef int (*corerpc_request)(void *ctx, void *);

typedef struct {
        nid_t nid;
        int coreid;
        uint64_t coremask;
        sockid_t sockid[CORE_MAX];

        int connecting;
        struct list_head wait_list;
        corerpc_request request;
        int (*connected)(const sockid_t *);
} corenet_maping_t;

int corenet_maping_init(uint64_t mask);
void corenet_maping_destroy(corenet_maping_t **maping);
int corenet_maping_connected(const nid_t *nid, const sockid_t *sockid);
void corenet_maping_closeall(const nid_t *nid, const sockid_t *sockid);
void corenet_maping_close(const nid_t *nid, const sockid_t *sockid);
int corenet_maping(void *core, const coreid_t *coreid, sockid_t *sockid);

int corenet_maping_register(uint64_t coremask);
void corenet_maping_check(const ltg_net_info_t *info);
int corenet_maping_offline(uint64_t coremask);

#endif
