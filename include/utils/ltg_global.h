#ifndef __LTG_GLOBAL_H__
#define __LTG_GLOBAL_H__

#include "ltg_id.h"

#define LNET_PORT_NULL ((uint32_t)-1)

typedef struct {
        nid_t local_nid;
        //uint32_t port;
        uint32_t master_magic;
} ltg_global_t;

extern ltg_global_t ltg_global;

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
        return &ltg_global.local_nid;
}

static inline void net_setnid(const nid_t *nid)
{
        ltg_global.local_nid = *nid;
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
        return ltg_global.local_nid.id;
}
#endif
