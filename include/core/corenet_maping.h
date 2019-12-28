#ifndef __CORENET_MAPING_H__
#define __CORENET_MAPING_H__

#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

#define CORENET_DEV_MAX 10

typedef int (*corerpc_send)(void *ctx, void *);

typedef struct {
        nid_t nid;
        ltg_spinlock_t lock;
        struct list_head list;
        uint64_t coremask;
        char loading;
        corerpc_send send;
        int (*connected)(const sockid_t *);
        sockid_t sockid[CORE_MAX];
} corenet_maping_t;

int corenet_maping_init();
int corenet_maping(void *core, const coreid_t *coreid, sockid_t *sockid);
void corenet_maping_close(const nid_t *nid, const sockid_t *sockid);
void corenet_maping_check(const ltg_net_info_t *info);
void corenet_maping_destroy(corenet_maping_t **maping);
int corenet_maping_register(uint64_t coremask);

#endif
