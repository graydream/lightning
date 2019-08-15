#ifndef __LNET_RPC_H__
#define __LNET_RPC_H__

#include <stdint.h>

#include "ltg_net.h"
#include "ltg_utils.h"

/* rpc_lib.c */
int rpc_init(const char *name);
int rpc_destroy(void);

/* rpc_passive.c */
int rpc_passive(uint32_t port);
int rpc_start();
void rpc_request_register(int type, net_request_handler handler, void *context);
void stdrpc_reply_tcp(void *ctx, void *arg);

/* stdrpc_reply.c */
void stdrpc_reply_error(const sockid_t *sockid, const msgid_t *msgid, int _error);
void stdrpc_reply_error_prep(const msgid_t *msgid, ltgbuf_t *buf, int _error);
void stdrpc_reply(const sockid_t *sockid, const msgid_t *msgid,
               const void *_buf, int len);
void stdrpc_reply1(const sockid_t *sockid, const msgid_t *msgid, ltgbuf_t *_buf);
void stdrpc_reply_init_prep(const msgid_t *msgid, ltgbuf_t *buf, ltgbuf_t *data, int flag);

/* rpc_xnect.c */
int rpc_getinfo(char *infobuf, uint32_t *infobuflen);

/* rpc_request.c */

int stdrpc_request_wait3(const char *name, const coreid_t *coreid, const void *request,
                      int reqlen, const ltgbuf_t *wbuf, ltgbuf_t *rbuf, int msg_type,
                      int priority, int timeout);
int stdrpc_request_wait(const char *name, const nid_t *nid, const void *request,
                     int reqlen, void *reply, int *replen, int msg_type,
                     int priority, int timeout);

int stdrpc_request_wait_sock(const char *name, const net_handle_t *nh, const void *request,
                          int reqlen, void *reply, int *replen, int msg_type,
                          int priority, int timeout);
int rpc_request_prep(ltgbuf_t *buf, const msgid_t *msgid, const void *request,
                     int reqlen, const ltgbuf_t *data, int prog, int merge, int priority);

#endif
