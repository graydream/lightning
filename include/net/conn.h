#ifndef __CONN_H__
#define __CONN_H__

#include "net_lib.h"

int conn_init();
int conn_scan();
int conn_register();
int conn_getinfo(const nid_t *nid, ltg_net_info_t *info);

#endif
