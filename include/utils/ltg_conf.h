#ifndef __LTG_CONF_H__
#define __LTG_CONF_H__

#include <unistd.h>
#include <stdint.h>

#define MAX_PATH_LEN    (1024 * 4)
#define MAX_NAME_LEN    (256)
#define MAX_INFO_LEN    (512)

#define MAX_BUF_LEN     (1024 * 4)

#define PAGE_SIZE (1024 * 4)

#define MAX_LINE_LEN    (1024 * 2)

typedef unsigned long long LLU;
typedef long long LLD;

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#define BUFFER_SEG_SIZE (2 * 1024 * 1024)
#define RDMA_MESSAGE_SIZE 512

#define IO_MAX (1024 * 1024 * 4)

#define MAX_SGE  (IO_MAX / BUFFER_SEG_SIZE + 1)
#define RDMA_INFO_SIZE 1024

#define LNET_SERVICE_BASE 49152
#define LNET_SERVICE_RANGE (65535 - 49152)

#define NODEID_MAX (INT16_MAX)

#define MAXSIZE 256

#define SCHEDULE_MAX  (1024 * 2)

#define MAX_MSG_SIZE (512)
#define UUID_LEN        (64)
#define MAX_NET_COUNT 12

typedef struct {
        int count;
        struct {
                uint32_t network;
                uint32_t mask;
        } network[MAX_NET_COUNT];
} ltg_netconf_t;

/* global configure */
struct ltgconf_t {
        char system_name[MAXSIZE];
        char workdir[MAXSIZE];
        int coredump;
        int restart;
        int rdma;
        int testing;
        int maxcore;
        int rpc_timeout;
        int backtrace;
        int coreflag;
        int tls;

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
};

extern struct ltgconf_t ltgconf;
extern ltg_netconf_t ltg_netconf;

#define SOCKID_NORMAL 10
#define SOCKID_CORENET 20

#define ENABLE_HUGEPAGE 1

#define ENABLE_ANALYSIS 0

#endif
