#ifndef __NET_TABLE_H__
#define __NET_TABLE_H__

#include "net_lib.h"

typedef enum {
        NETABLE_NULL,
        NETABLE_CONN,
        NETABLE_DEAD,
} netstatus_t;

typedef struct {
        net_handle_t sock;
} socklist_t;

typedef struct {
        uint32_t prev;
        uint32_t now;
} ltime_t;

typedef struct __connection {
        net_handle_t nh;
        netstatus_t status;
        char lname[MAX_NAME_LEN];
        ltime_t ltime;
        uint32_t timeout;
        net_handle_t sock;
} ltg_net_conn_t;

int netable_init(int daemon);
int netable_accept(const ltg_net_info_t *info, const net_handle_t *sock);
int netable_connect_info(net_handle_t *nh, const ltg_net_info_t *info, int force);
int netable_connected(const nid_t *nid);
void netable_close(const nid_t *nid, const char *resion, const time_t *ltime);
const char *netable_rname(const nid_t *nid);
int netable_getsock(const nid_t *nid, sockid_t *sockid);
time_t netable_conn_time(const nid_t *nid);
void netable_iterate(void);

#endif
