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

#define ENABLE_TCP_THREAD 0

#if ENABLE_RDMA

typedef struct {
        struct ibv_context *ibv_verbs;
        struct ibv_cq *cq;
        struct ibv_mr *mr;
        // int ref;
} rdma_info_t;

#define RDMA_INFO_DUMP(info) do { \
        DINFO("rdma_info %p verbs %p cq %p mr %p\n",  \
               (info), \
               (info)->ibv_verbs, \
               (info)->cq, \
               (info)->mr \
               ); \
} while(0)

typedef struct {
        int node_loc;

        int ref;
        int qp_ref;

        int is_closing;
        int is_connected;

        core_t *core;

        struct rdma_cm_id *cm_id;
        struct ibv_mr *mr;
        struct ibv_mr *iov_mr;
        struct ibv_qp *qp;
        struct ibv_pd *pd;

        void *private_mem;
        void *iov_addr;
} rdma_conn_t;

#define RDMA_CONN_DUMP(conn) do { \
        DINFO("rdma_conn[%d] %p core %p close %d conn %d ref %d/%d cm_id %p qp %p addr %p\n", \
               (conn)->node_loc, \
               (conn), \
               (conn)->core, \
               (conn)->is_closing, \
               (conn)->is_connected, \
               (conn)->ref, \
               (conn)->qp_ref, \
               (conn)->cm_id, \
               (conn)->qp, \
               (conn)->iov_addr \
               ); \
} while(0)

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
        ltg_spinlock_t lock;
        void *ctx;

        core_exec exec;
        func_t reset;
        func_t recv;
        func_t check;

        ltgbuf_t send_buf;
        ltgbuf_t recv_buf;
        ltgbuf_t queue_buf;
        struct list_head send_list;
        int ref;
        int closed;

#if ENABLE_TCP_THREAD
        plock_t rwlock;
#endif

        char name[MAX_NAME_LEN / 2];
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

#define CORENET_RDMA_NODE_DUMP(node) do { \
        DINFO("rdma_node %p sock %d conn %p ref %d in_use %d send %d ctx %p\n",  \
               (node), \
               (node)->sockid.sd, \
               &(node)->handler, \
               (node)->ref, \
               (node)->in_use, \
               (node)->send_count, \
               (node)->ctx \
               ); \
} while(0)

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
	int size;
        ltg_spinlock_t lock;
        time_t last_check;
        struct list_head check_list;
        struct list_head add_list;
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

int corenet_tcp_add(corenet_tcp_t *corenet, const sockid_t *sockid, void *ctx,
                    core_exec exec, func_t reset, func_t check, func_t recv, const char *name);
void corenet_tcp_close(const sockid_t *sockid);

void corenet_tcp_check_add();
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
void *rdma_register_mgr(void* pd, void* buf, size_t size);

int corenet_rdma_add(core_t *core, sockid_t *sockid, void *ctx,
                     core_exec exec, core_exec1 exec1, func_t reset,
                     func_t check, func_t recv, rdma_conn_t **_handler);

void corenet_rdma_close(rdma_conn_t *rdma_handler);

void corenet_rdma_put(rdma_conn_t *rdma_handler);
void corenet_rdma_get(rdma_conn_t *rdma_handler, int n);

void corenet_rdma_check();

int corenet_rdma_connect(uint32_t addr, uint32_t port, sockid_t *sockid);
int corenet_rdma_connected(const sockid_t *sockid);
void corenet_rdma_established(struct rdma_cm_event *ev, void *core);
void corenet_rdma_disconnected(struct rdma_cm_event *ev, void *core);

int corenet_rdma_post_recv(void *ptr);
int corenet_rdma_send(const sockid_t *sockid, ltgbuf_t *buf, void **addr, uint32_t rkey, uint32_t size,
                      rdma_req_t *(*build_req)(rdma_conn_t *rdma_handler, ltgbuf_t *buf, void **addr,
                                               uint32_t rkey, uint32_t size));
void corenet_rdma_commit(void *rdma_net);

rdma_req_t *build_post_send_req(rdma_conn_t *rdma_handler, ltgbuf_t *buf, void **addr, uint32_t rkey, uint32_t size);
rdma_req_t *build_rdma_read_req(rdma_conn_t *rdma_handler, ltgbuf_t *buf, void **addr, uint32_t rkey, uint32_t size);
rdma_req_t *build_rdma_write_req(rdma_conn_t *rdma_handler, ltgbuf_t *buf, void **addr, uint32_t rkey, uint32_t size);

int corenet_rdma_poll(__corenet_t *corenet);

void corenet_rdma_timewait_exit(struct rdma_cm_event *ev, void *core);

int corenet_rdma_evt_channel_init();

// server-side
int corenet_rdma_passive(uint32_t *port, int cpu_index);
int corenet_rdma_listen_by_channel(int cpu_idx, uint32_t port);
void corenet_rdma_connect_request(struct rdma_cm_event *ev, void *core);

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

int corenet_attach(void *_corenet, const sockid_t *sockid, void *ctx,
                   core_exec exec, func_t reset, func_t check, func_t recv,
                   const char *name);
int corenet_send(void *ctx, const sockid_t *sockid, ltgbuf_t *buf);

#endif
