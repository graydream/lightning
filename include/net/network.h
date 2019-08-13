#ifndef __NETWORK_H__
#define __NETWORK_H__

#include <stdio.h>

#include "net_lib.h"
#include "ltg_utils.h"
#include "ltg_net.h"

/*network.c*/
int network_init(void);

int network_connect(const nid_t *nid, time_t *_ltime, int _timeout, int force);
int network_connect_wait(const nid_t *nid, time_t *_ltime, int _timeout, int force);
int network_ltime(const nid_t *nid, time_t *ltime);
time_t network_ltime1(const nid_t *nid);

const char *network_rname(const nid_t *nid);
int network_rname1(const nid_t *nid, char *name);
void network_close(const nid_t *nid, const char *why, const time_t *ltime);


#endif
