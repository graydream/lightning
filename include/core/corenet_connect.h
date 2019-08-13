#ifndef __LTG_CORENET_CONNECT_H__
#define __LTG_CORENET_CONNECT_H__

#include "ltg_net.h"

int corenet_tcp_connect(const coreid_t *coreid, uint32_t addr, uint32_t port, sockid_t *sockid);
int corenet_tcp_passive(const coreid_t *coreid, uint32_t *_port, int *_sd);
int corenet_tcp_getaddr(uint32_t port, corenet_addr_t *addr);

#if 0
int corenet_rdma_connect(uint32_t addr, uint32_t port, sockid_t *sockid);
int corenet_rdma_passive(uint32_t port, int cpu_index);
#endif
/** @file 不同节点上多个core间的RPC.
 *
 * CORE地址： <nid, core hash>
 *
 * 本地缓存了CORE地址到sockid的映射关系。
 *
 * 如需要支持跨集群，还需把集群ID编入CORE地址。
 */

#if 0
int corenet_connect_host(const char *host, sockid_t *sockid);
#endif


#endif
