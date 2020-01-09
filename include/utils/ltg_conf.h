#ifndef __LTG_CONF_H__
#define __LTG_CONF_H__

#include <unistd.h>
#include <stdint.h>
#include "ltg_def.h"
#include "ltg_id.h"

typedef struct {
        int count;
        struct {
                uint32_t network;
                uint32_t mask;
        } network[MAX_NET_COUNT];
} ltg_netconf_t;

/* global configure */
typedef struct {
        char system_name[MAXSIZE];
        char service_name[MAXSIZE];
        char workdir[MAXSIZE];
        nid_t nid;
        int coredump;
        int restart;
        int rdma;
        int testing;
        int maxcore;
        int rpc_timeout;
        int backtrace;
        int coreflag;

        int polling_timeout;
        uint64_t coremask;
        int use_huge;
        int daemon;
        
        int wmem_max;
        int rmem_max;
        int solomode;
        int performance_analysis;
        int lease_timeout;
        int hb_timeout;
        int hb_retry;
        int tcp_discovery;
} ltgconf_t;

extern ltgconf_t ltgconf_global;
extern ltg_netconf_t ltg_netconf_global;
extern ltg_netconf_t ltg_netconf_manage;

#endif
