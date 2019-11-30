#ifndef __LNET_SOCK_H__
#define __LNET_SOCK_H__

#include <stdint.h>
#include <poll.h>

#include "sock_buffer.h"
#include "net_proto.h"
#include "ltg_utils.h"
#include "ltg_net.h"

#pragma pack(8)
typedef struct {
        uint32_t addr;
        uint32_t port;
} sock_info_t;
#pragma pack()

typedef struct {
        net_handle_t nh;
        nid_t *nid;
        nid_t __nid__;
        net_proto_t proto;
        uint32_t align;

        uint64_t send_total;
        uint64_t recv_total;

        double limit_rate;
        int delay;
        int sendclose;/*close after sent*/
        int used;

        sock_wltgbuf_t wbuf;
        sock_rltgbuf_t rbuf;
} ltg_sock_conn_t;

/* sock_passive.c */
extern int sock_accept(net_handle_t *, int srv_sd, int tuning, int nonblock);
extern int sock_getinfo(uint32_t *info_count, sock_info_t *,
                        uint32_t info_count_max, uint32_t port,
                        const ltg_netconf_t *filter);
extern int sock_setblock(int sd);
extern int sock_setnonblock(int sd);

/* sock_xnect.c */
extern int sock_init(ltg_sock_conn_t *sock, sock_info_t *info);
extern int sock_info2sock(net_handle_t *nh, const sock_info_t *, int nonblock, int timeout);
extern int sock_close(ltg_sock_conn_t *);
extern int sock_sdclose(int sd);

int sock_poll_sd(int sd, uint64_t usec, short event);
#endif
