#ifndef __LTG_H__
#define __LTG_H__

#include "3part.h"
#include "ltg_utils.h"
#include "ltg_mem.h"
#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

int ltg_conf_init(ltgconf_t *ltgconf);
int ltg_init(const ltgconf_t *ltgconf, const ltg_netconf_t *ltgnet_manage,
             const ltg_netconf_t *ltgnet_conf);
void ltg_net_add(uint32_t network, uint32_t netmask);

#endif
