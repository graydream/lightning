#ifndef __LTG_CORERPC_H__
#define __LTG_CORERPC_H__

#include "ltg_core.h"
#include "ltg_net.h"
#include "ltg_rpc.h"

/**
 * @file CORERPC
 *
 * 上层采用nid作为节点标识，
 * 发送端：(hash, nid)通过cornet_mapping机制转化为sockid。
 * 接收端
 *
 * core hash用于定位core，集群中所有节点上具有相同hash值的core构成corenet。
 * 这种结构关系在整个运行期间要保持稳定。
 *
 * 每个core有自己的corenet： sd -> corenet_node_t
 *
 * 网络层分为三层：
 * - rpc
 * - net
 * - sock
 *
 * 有三类RPC：
 * - minirpc
 * - rpc
 * - corerpc (用于core之间的通信）
 *
 * @note 网络层经过了协程改造
 * @note 两个实体之间采用单连接
 * @note 节点上的core数量是否需要一样？
 */

typedef struct {
        int running;
        sockid_t sockid;
        coreid_t coreid;
        coreid_t local;
        void *corenet;
} corerpc_ctx_t;

void corerpc_register(int type, request_get_handler handler, void *context);

int corerpc_postwait(const char *name,
                     const coreid_t *coreid, const void *request,
                     int reqlen, int replen, const ltgbuf_t *wbuf, ltgbuf_t *rbuf,
                     int msg_type, int msg_size, int group, int timeout);

int corerpc_postwait1(const char *name,
                      const coreid_t *coreid, const void *request,
                      int reqlen,  void *reply, int *replen,
                      int msg_type, int group, int timeout);

int corerpc_postwait_sock(const char *name, const coreid_t *coreid,
                          const sockid_t *sockid, const void *request,
                          int reqlen, int msg_type,
                          int group, int timeout);

void corerpc_reply_buffer(const sockid_t *sockid, const msgid_t *msgid, ltgbuf_t *_buf);
void corerpc_reply_error(const sockid_t *sockid, const msgid_t *msgid, int _error);

void corerpc_reply_rdma(void *ctx, void *arg);
void corerpc_reply_tcp(void *ctx, void *arg);

int corerpc_tcp_recv(void *ctx, void *buf, int *count);

void corerpc_scan(void *ctx);

// callback
void corerpc_close(void *ctx);
void corerpc_reset(const sockid_t *sockid);
void corerpc_destroy(rpc_table_t **_rpc_table);


//rpc table
int corerpc_init(uint64_t mask);

#if ENABLE_RDMA

void corerpc_rdma_reset(const sockid_t *sockid);
int corerpc_rdma_recv_msg(void *_ctx, void *iov, int *_count);
int corerpc_rdma_recv_data(void *_ctx, void *_msg_buf);

#endif


int corerpc_rdma_request(void *ctx, void *_op);
int corerpc_tcp_request(void *ctx, void *_op);

#endif
