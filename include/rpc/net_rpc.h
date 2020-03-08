#ifndef __NET_RPC_H__
#define __NET_RPC_H__

#include "ltg_net.h"

typedef struct {
        uint32_t len; /*length of the info*/
        coreid_t coreid;
        uint16_t info_count;       /**< network interface number */
        sock_info_t info[0];  /**< host byte order */
} corenet_addr_t;

int net_rpc_coreinfo(const coreid_t *coreid, corenet_addr_t *addr);
int net_rpc_heartbeat(const sockid_t *sockid, uint64_t seq);

int net_rpc_coremask(const nid_t *nid, uint64_t *mask);
int net_rpc_hello1(const sockid_t *sockid, uint64_t seq);
int net_rpc_hello2(const coreid_t *coreid, const sockid_t *sockid, uint64_t seq);
int net_rpc_init(void);

#endif
