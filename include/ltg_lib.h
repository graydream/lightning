#ifndef __LTG_H__
#define __LTG_H__

#include "3part.h"
#include "ltg_utils.h"
#include "ltg_mem.h"
#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

#if 0
int ltg_conf_init(const char *sys_name, const char *srv_name, const char *workdir,
                  uint64_t coremask, int rpc_timeout, int polling_timeout, int rdma,
                  int performance_analysis, int use_huge,
                  int backtrace, int daemon, int coreflag);
#endif

int ltg_conf_init(ltgconf_t *ltgconf, ltg_netconf_t *ltgnet_conf);
int ltg_init(const ltgconf_t *ltgconf, const ltg_netconf_t *ltgnet_conf);
void ltg_net_add(uint32_t network, uint32_t netmask);

#endif
