#ifndef __NET_GLOBAL_H__
#define __NET_GLOBAL_H__

#include <uuid/uuid.h>
#include "sdevent.h"
#include "ltg_utils.h"

#define LNET_PORT_NULL ((uint32_t)-1)

typedef struct {
        int inited;
        net_proto_t op;
        nid_t local_nid;

        char name[MAX_PATH_LEN];
        char home[MAX_PATH_LEN];
        uint32_t seq; /*local seq*/
        uint32_t port;
        uint32_t uptime;
        time_t info_time;
        char info_local[MAX_INFO_LEN];
        int daemon;
        uint32_t master_magic;
} net_global_t;

/*init in net_lib.c*/

extern net_global_t ng;

static inline int net_isnull(const nid_t *nid)
{
        if (nid == NULL)
            return 1;

        if (nid->id == 0)
                return 1;
        else
                return 0;
}

static inline const nid_t *net_getnid()
{
        return &ng.local_nid;
}

static inline void net_setnid(const nid_t *nid)
{
        ng.local_nid = *nid;
}

static inline int net_islocal(const nid_t *nid)
{
        if (net_isnull(nid))
                return 0;

        if (nid_cmp(nid, net_getnid()) == 0)
                return 1;
        else
                return 0;
}

//todo 用net_isnull 替代
static inline int is_null(const nid_t *nid)
{
        return net_isnull(nid);
}

//todo 用net_islocal 替代
static inline int is_local(const nid_t *nid)
{
        return net_islocal(nid);
}

static inline uint64_t net_getnodeid(void)
{
        return ng.local_nid.id;
}
#endif
