#ifndef __CONN_H__
#define __CONN_H__

#include "net_lib.h"

int conn_init();
int conn_retry(const nid_t *_nid);
int conn_register();
int conn_getinfo(const nid_t *nid, ltg_net_info_t *info);
int conn_setinfo();
int conn_online(const nid_t *nid, int timeout);
int conn_faultdomain(int *_total, int *_online);

#endif
