#ifndef __LTG_CORENET_CONNECT_H__
#define __LTG_CORENET_CONNECT_H__

#include "ltg_net.h"

int corenet_tcp_connect(const coreid_t *coreid, uint32_t addr, uint32_t port, sockid_t *sockid);
int corenet_tcp_passive(const coreid_t *coreid, uint32_t *_port, int *_sd);

#endif
