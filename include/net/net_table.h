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
        char nodeid[MAX_NODEID_LEN];
        char lname[MAX_NAME_LEN];
        uint64_t load;
        ltime_t ltime;
        time_t update;
        time_t last_retry;
        uint32_t timeout;
        net_handle_t sock;
} ltg_net_conn_t;

int netable_init(int daemon);

int netable_accept(const ltg_net_info_t *info, const net_handle_t *sock);

int netable_connect_info(net_handle_t *nh, const ltg_net_info_t *info, int force);
int netable_nodeid(const nid_t *nid, char *nodeid);

int netable_connected(const nid_t *nid);
int netable_connectable(const nid_t *nid, int force);

void netable_close(const nid_t *nid, const char *resion, const time_t *ltime);

const char *netable_rname_nid(const nid_t *nid);
int netable_rname1(const nid_t *nid, char *name);

int netable_getsock(const nid_t *nid, sockid_t *sockid);

time_t netable_conn_time(const nid_t *nid);

void netable_sort(nid_t *nid, int count);

void netable_load_update(const nid_t *nid, uint64_t load);
int netable_update_retry(const nid_t *nid);

void netable_iterate(void);
void netable_update(const nid_t *nid);
time_t netable_last_update(const nid_t *nid);

int netable_cores(const nid_t *nid, int *from, int *to);


#endif
