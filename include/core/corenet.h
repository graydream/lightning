#ifndef __LTG_CORENET_H__
#define __LTG_CORENET_H__

#include <sys/epoll.h>
#include <semaphore.h>
#include <pthread.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

#define ENABLE_RDMA 1

#if ENABLE_RDMA

#define CMID_DUMP_L(LEVEL, cmid) do { \
        struct sockaddr_in *__sa = (struct sockaddr_in *)(&(cmid)->route.addr.src_addr); \
        struct sockaddr_in *__da = (struct sockaddr_in *)(&(cmid)->route.addr.dst_addr); \
        LEVEL("cmid %p ctx %p local %s:%d verbs %p cq %p/%p chan %p ev %p qp %p pd %p\n", \
               (cmid), \
               (cmid)->context, \
               inet_ntoa(__sa->sin_addr), \
               ntohs(__sa->sin_port), \
               (cmid)->verbs, \
               (cmid)->send_cq, \
               (cmid)->recv_cq, \
               (cmid)->channel, \
               (cmid)->event, \
               (cmid)->qp, \
               (cmid)->pd); \
        LEVEL("cmid %p ctx %p remote %s:%d verbs %p cq %p/%p chan %p ev %p qp %p pd %p\n", \
               (cmid), \
               (cmid)->context, \
               inet_ntoa(__da->sin_addr), \
               ntohs(__da->sin_port), \
               (cmid)->verbs, \
               (cmid)->send_cq, \
               (cmid)->recv_cq, \
               (cmid)->channel, \
               (cmid)->event, \
               (cmid)->qp, \
               (cmid)->pd); \
} while(0)

#define CMID_DUMP(cmid) CMID_DUMP_L(DBUG, cmid);

#define IBV_MR_DUMP_L(LEVEL, mr) do { \
        LEVEL("mr %p addr %p len %lu lkey %u rkey %u pd %p context %p\n", \
              (mr), \
              (mr)->addr, \
              (mr)->length, \
              (mr)->lkey, \
              (mr)->rkey, \
              (mr)->pd, \
              (mr)->context \
              ); \
} while(0)

#define IBV_MR_DUMP(mr) IBV_MR_DUMP_L(DBUG, mr)

#define IBV_QP_DUMP_L(LEVEL, qp) do { \
        LEVEL("qp %p cq %p/%p qp_num %u state %d type %d pd %p context %p\n", \
              (qp), \
              (qp)->send_cq, \
              (qp)->recv_cq, \
              (qp)->qp_num, \
              (qp)->state, \
              (qp)->qp_type, \
              (qp)->pd, \
              (qp)->context \
              ); \
} while(0)

#define IBV_QP_DUMP(qp) IBV_QP_DUMP_L(DBUG, qp)

#define IBV_CQ_DUMP_L(LEVEL, cq) do { \
        LEVEL("cq %p cge %d handle %u context %p\n", \
              (cq), \
              (cq)->cqe, \
              (cq)->handle, \
              (cq)->context \
              ); \
} while(0)

#define IBV_CQ_DUMP(cq) IBV_CQ_DUMP_L(DBUG, cq)

typedef struct {
        struct ibv_context *ibv_verbs;
        struct ibv_cq *cq;
        struct ibv_pd *pd;
        struct ibv_mr *mr;
        // int ref;

        uint32_t nr_conn;

        uint64_t nr_success;
        uint64_t nr_flush;
        uint64_t nr_other;
} rdma_info_t;

#define RDMA_INFO_DUMP_L(LEVEL, info) do { \
        LEVEL("rdma_info %p conn %u nr %ju/%ju/%ju verbs %p cq %p pd %p mr %p\n",  \
               (info), \
               (info)->nr_conn, \
               (info)->nr_success, \
               (info)->nr_flush, \
               (info)->nr_other, \
               (info)->ibv_verbs, \
               (info)->cq, \
               (info)->pd, \
               (info)->mr \
               ); \
} while(0)

#define RDMA_INFO_DUMP(info) RDMA_INFO_DUMP_L(DBUG, info)

typedef struct {
        int node_loc;

        int ref;
        int qp_ref;

        int is_closing;
        int is_connected;

        core_t *core;

        struct rdma_cm_id *cm_id;
        struct ibv_cq *cq;
        struct ibv_pd *pd;
        struct ibv_mr *mr;
        struct ibv_mr *iov_mr;
        struct ibv_qp *qp;

        void *private_mem;
        void *iov_addr;

        // client or server
        int type;
        struct rdma_event_channel *channel;
        rdma_info_t *dev;

        uint64_t nr_get;
        uint64_t nr_ack;

        uint64_t nr_success;
        uint64_t nr_flush;
        uint64_t nr_other;
} rdma_conn_t;

#define RDMA_CONN_DUMP_L3(LEVEL, env, conn) do { \
        struct ibv_cq *cq = (conn)->dev ? (conn)->dev->cq : NULL; \
        LEVEL("%s: rdma_conn[%d] %p conn %d/%d ref %d/%d ack %ju/%ju nr %ju/%ju/%ju rinfo %p cq %p pd %p cmid %p type %d chan %p qp %p mr %p/%p addr %p core %p\n", \
               (env), \
               (conn)->node_loc, \
               (conn), \
               (conn)->is_connected, \
               (conn)->is_closing, \
               (conn)->ref, \
               (conn)->qp_ref, \
               (conn)->nr_get, \
               (conn)->nr_ack, \
               (conn)->nr_success, \
               (conn)->nr_flush, \
               (conn)->nr_other, \
               (conn)->dev, \
               cq, \
               (conn)->pd, \
               (conn)->cm_id, \
               (conn)->type, \
               (conn)->channel, \
               (conn)->qp, \
               (conn)->mr, \
               (conn)->iov_mr, \
               (conn)->iov_addr, \
               (conn)->core \
               ); \
} while(0)

#define RDMA_CONN_DUMP_L(LEVEL, conn) RDMA_CONN_DUMP_L3(LEVEL, "", conn)

#define RDMA_CONN_DUMP(conn) RDMA_CONN_DUMP_L(DBUG, conn);

typedef struct {
        uint32_t mode:4;
        uint32_t err:2;
        uint32_t ref:8;
        uint32_t n:16;
        rdma_conn_t    *rdma_handler;
        ltgbuf_t msg_buf;
        union {
                struct ibv_recv_wr rr;
                struct ibv_send_wr sr[MAX_SGE];
        } wr;
        struct ibv_sge sge[MAX_SGE];
} rdma_req_t;

#define RDMA_REQ_DUMP_L(LEVEL, req) do { \
        LEVEL("rdma_req %p m %u h %p ref %u buf %ju used %d\n", \
               (req), \
               (req)->mode, \
               (req)->rdma_handler, \
               (req)->ref, \
               (req)->msg_buf.len, \
               (req)->msg_buf.used \
        ); \
} while(0)

#define RDMA_REQ_DUMP(req) RDMA_REQ_DUMP_L(DBUG, req)

#define CORENET_RDMA_ON_ACTIVE_WAIT FALSE

enum corenet_rdma_op_code {
        RDMA_RECV_MSG = 0,
        RDMA_SEND_MSG,
        RDMA_WRITE,
        RDMA_READ,
        RDMA_OP_END,
};

#endif

#if ENABLE_TCP_THREAD
#define CORE_IOV_MAX ((uint64_t)1024 * 10)
#else
#define CORE_IOV_MAX (1024 * 1)
#endif
#define DEFAULT_MH_NUM 1024
#define MAX_REQ_NUM ((DEFAULT_MH_NUM) / 2)
#define EXTRA_SIZE (4)

typedef struct {
        struct list_head hook;
        int ev;
        sockid_t sockid;
        void *ctx;

        core_exec exec;
        func_t reset;
        func_t recv;
        func_t check;

        ltgbuf_t send_buf;
        ltgbuf_t recv_buf;

#if ENABLE_TCP_THREAD
        plock_t rwlock;
#endif
        char *name;
} corenet_tcp_node_t;

#if ENABLE_RDMA

typedef struct {
        struct list_head hook;

        sockid_t sockid;
        rdma_conn_t handler;

        int ev;
        int ref;
        int in_use;

        void *ctx;

        ltg_spinlock_t lock;

        core_exec exec;
        core_exec1 exec1;
        func_t reset;
        func_t recv;
        func_t check;

        struct ibv_send_wr head_sr;
        struct ibv_send_wr *last_sr;

        int send_count;
        struct list_head send_list;
} corenet_rdma_node_t;

#define CORENET_RDMA_NODE_DUMP_L(LEVEL, node) do { \
        LEVEL("rdma_node %p sock %d conn %p ref %d in_use %d send %d ctx %p\n",  \
               (node), \
               (node)->sockid.sd, \
               &(node)->handler, \
               (node)->ref, \
               (node)->in_use, \
               (node)->send_count, \
               (node)->ctx \
               ); \
} while(0)

#define CORENET_RDMA_NODE_DUMP(node) CORENET_RDMA_NODE_DUMP_L(DBUG, node);

#endif

typedef struct {
        struct list_head hook;
        sockid_t sockid;
        core_exec exec;
        ltgbuf_t queue_buf;
        struct ringbuf *send_buf;
        struct ringbuf *recv_buf;
} corenet_ring_ltgbuf_t;

typedef struct {
        int epoll_fd;
	int count;
        ltg_spinlock_t lock;
        time_t last_check;
        struct list_head check_list;
        struct list_head forward_list;
        uint32_t figerprint;
} corenet_t;

#define MAX_RDMA_DEV 4

typedef struct {
        void *ring_net;
#if ENABLE_RDMA
        int   dev_count;
        void *rdma_net;
        rdma_info_t dev_list[MAX_RDMA_DEV];
#endif
        void *tcp_net;
        uint32_t port;
        time_t last_check;
        coreid_t coreid;
        int sd;
} __corenet_t __attribute__((__aligned__(CACHE_LINE_SIZE)));;

typedef struct {
        struct list_head poll_list;
        int size;
        corenet_ring_ltgbuf_t array[0];
} corenet_ring_t;

typedef struct {
        corenet_t corenet;
        corenet_rdma_node_t array[0];
} corenet_rdma_t;

typedef struct {
        corenet_t corenet;
#if !ENABLE_TCP_THREAD
        struct iovec iov[CORE_IOV_MAX]; //iov for send/recv
#endif
        corenet_tcp_node_t array[0];
} corenet_tcp_t;

int corenet_tcp_init(int max, corenet_tcp_t **corenet);
void corenet_tcp_destroy();

int corenet_tcp_attach(int coreid, const sockid_t *sockid, void *ctx,
                       core_exec exec, func_t reset, func_t check,
                       func_t recv, const char *name);
int corenet_tcp_add(corenet_tcp_t *corenet, const sockid_t *sockid, void *ctx,
                    core_exec exec, func_t reset, func_t check, func_t recv, const char *name);
void corenet_tcp_close(const sockid_t *sockid);

void corenet_tcp_check();

int corenet_tcp_connected(const sockid_t *sockid);

int corenet_tcp_poll(void *ctx, int tmo);
int corenet_tcp_send(void *ctx, const sockid_t *sockid, ltgbuf_t *buf);
void corenet_tcp_commit(void *ctx);

#if ENABLE_RDMA
// below is RDMA transfer

int corenet_rdma_dev_create(rdma_info_t * res);
int corenet_rdma_init(int max, corenet_rdma_t **_corenet);

int rdma_alloc_pd(rdma_info_t * res);
int rdma_create_cq(rdma_info_t *res, int ib_port);

//struct ibv_mr *rdma_get_mr();
void *rdma_get_mr_addr();
void *rdma_register_mr(void* pd, void* buf, size_t size);

int corenet_rdma_add(core_t *core, sockid_t *sockid, void *ctx,
                     core_exec exec, core_exec1 exec1, func_t reset,
                     func_t check, func_t recv, rdma_conn_t **_handler);

void corenet_rdma_close(rdma_conn_t *rdma_handler, const char *caller);

void corenet_rdma_put(rdma_conn_t *rdma_handler, const char *caller, int verbose);
void corenet_rdma_get(rdma_conn_t *rdma_handler, int n, const char *caller, int verbose);

void corenet_rdma_check();

int corenet_rdma_connect(uint32_t addr, uint32_t port, sockid_t *sockid);
int corenet_rdma_connected(const sockid_t *sockid);

void corenet_rdma_connect_request(struct rdma_cm_event *ev, void *core);
void corenet_rdma_established(struct rdma_cm_event *ev, void *core);
void corenet_rdma_disconnected(struct rdma_cm_event *ev, void *core);
void corenet_rdma_timewait_exit(struct rdma_cm_event *ev, void *core);

int corenet_rdma_post_recv(void *ptr);
typedef rdma_req_t *(*req_build_func)(rdma_conn_t *rdma_handler, ltgbuf_t *buf,
                                 void **addr, uint32_t rkey, uint32_t size);
int corenet_rdma_send(const sockid_t *sockid, ltgbuf_t *buf, void **addr,
                      uint32_t rkey, uint32_t size,
                      req_build_func);
void corenet_rdma_commit(void *rdma_net);

rdma_req_t *build_post_send_req(rdma_conn_t *rdma_handler, ltgbuf_t *buf, void **addr, uint32_t rkey, uint32_t size);
rdma_req_t *build_rdma_read_req(rdma_conn_t *rdma_handler, ltgbuf_t *buf, void **addr, uint32_t rkey, uint32_t size);
rdma_req_t *build_rdma_write_req(rdma_conn_t *rdma_handler, ltgbuf_t *buf, void **addr, uint32_t rkey, uint32_t size);

int corenet_rdma_poll(__corenet_t *corenet);

int corenet_rdma_evt_channel_init();

// server-side
int corenet_rdma_passive(uint32_t *port, int cpu_index);
int corenet_rdma_listen_by_channel(int cpu_idx, uint32_t port);

// client-side
int corenet_rdma_connect_by_channel(const uint32_t addr, const uint32_t port, core_t *core, sockid_t *sockid);

// int corenet_rdma_on_passive_event(int cpu_idx);
int rdma_event_init();
int rdma_event_add(int fd, int type, int event, event_handle_t handler, void *data, void *core);
void rdma_handle_event(int fd, int type, int events __attribute__ ((unused)),
                       void *data __attribute__ ((unused)), void *core);

#endif

int corenet_init();

int corenet_getaddr(const coreid_t *coreid, corenet_addr_t *addr);
int corenet_register(uint64_t coremask);
void corenet_close(const sockid_t *sockid);

int corenet_send(void *ctx, const sockid_t *sockid, ltgbuf_t *buf);

#endif
