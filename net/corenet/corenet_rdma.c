

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#include <semaphore.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <sys/mman.h>
#include <numaif.h>
#define DBG_SUBSYS S_LTG_NET

#include "ltg_core.h"
#include "ltg_net.h"
#include "ltg_utils.h"

#define MAX_SEG_COUNT MAX_SGE
#define RDMA_CQ_POLL_COMP_OK 0
#define RDMA_CQ_POLL_COMP_EMPTY 1
#define RDMA_CQ_POLL_COMP_ERROR 2

/* poll CQ timeout in millisec (2 seconds) */
#define MAX_POLL_CQ_SIZE (512 * 1024)
#define MAX_POLL_CQ_TIMEOUT 2000
#define MAX_BUF_SIZE 512

#define RDMA_DEBUG 1

#define min_t(type, x, y) ({                    \
        type _min1 = (x);                       \
        type _min2 = (y);                       \
        _min1 < _min2 ? _min1 : _min2; })
//static __thread struct ibv_mr *gmr = NULL;
static struct rdma_event_channel **corenet_rdma_evt_channel;

typedef struct {
        struct list_head hook;
        sockid_t sockid;
        int cur_index;
        int end_index;
        int seg_count[MAX_BUF_SIZE];
        ltgbuf_t buf_list[MAX_BUF_SIZE];
} corenet_rdma_fwd_t;

typedef corenet_rdma_node_t corenet_node_t;
static uint16_t __seq__ = 1;

static void *__corenet_get()
{
        return core_tls_get(NULL, VARIABLE_CORENET_RDMA);
}

static void __corenet_rdma_checklist_add(corenet_rdma_t *corenet, corenet_node_t *node)
{
        int ret;

        ret = ltg_spin_lock(&corenet->corenet.lock);
        if (ret)
                UNIMPLEMENTED(__DUMP__);

        list_add_tail(&node->hook, &corenet->corenet.check_list);

        ltg_spin_unlock(&corenet->corenet.lock);
}

static void __corenet_rdma_checklist_del(corenet_rdma_t *corenet, corenet_node_t *node)
{
        int ret;

        ret = ltg_spin_lock(&corenet->corenet.lock);
        if (ret)
                UNIMPLEMENTED(__DUMP__);

        list_del(&node->hook);

        ltg_spin_unlock(&corenet->corenet.lock);
}

void corenet_rdma_check()
{
        int ret;
        time_t now;
        struct list_head *pos;
        corenet_node_t *node;
        corenet_rdma_t *__corenet_rdma__ = __corenet_get();

        now = gettime();
        if (now - __corenet_rdma__->corenet.last_check < 30) {
                return;
        }

        __corenet_rdma__->corenet.last_check = now;

        DINFO("corenet check\n");

        ret = ltg_spin_lock(&__corenet_rdma__->corenet.lock);
        if (ret)
                UNIMPLEMENTED(__DUMP__);

        list_for_each(pos, &__corenet_rdma__->corenet.check_list) {
                node = (void *)pos;
                node->check(node->ctx);
        }

        ltg_spin_unlock(&__corenet_rdma__->corenet.lock);
}

void __rdma_destroy_qp(struct rdma_cm_id *cm_id, const char *env)
{
        LTG_ASSERT(cm_id->qp != NULL);

        DINFO("env %s\n", env);
        CMID_DUMP_L(DINFO, cm_id);

        rdma_destroy_qp(cm_id);
}

static void __corenet_rdma_free_node(corenet_rdma_t *rdma_net, corenet_node_t *node)
{
        if (!list_empty(&node->hook)) {
                __corenet_rdma_checklist_del(rdma_net, node);
        }

        CORENET_RDMA_NODE_DUMP(node);
        RDMA_CONN_DUMP_L(DINFO, &node->handler);

        //  ltg_free((void **)&node->ctx);

        INIT_LIST_HEAD(&node->send_list);
        node->ev = 0;
        node->ctx = NULL;

        node->exec = NULL;
        node->exec1 = NULL;
        node->reset = NULL;
        node->recv = NULL;

        node->sockid.sd = -1;
        node->in_use = 0;

        node->send_count = 0;
        node->head_sr.next = NULL;
        node->last_sr = &node->head_sr;

        memset(&node->handler, 0x00, sizeof(rdma_conn_t));
}

static void __corenet_rdma_free_node1(core_t *_core, sockid_t *sockid)
{
        corenet_node_t *node = NULL;
        __corenet_t *_corenet = (__corenet_t *)_core->corenet;
        corenet_rdma_t *corenet = (corenet_rdma_t *)_corenet->rdma_net;

	node = &corenet->array[sockid->sd];

	if (node->send_count > 0) { 
		struct ibv_send_wr *sr = node->head_sr.next;
		rdma_req_t *req;
		int nr = 0;
		while (sr) {
			req = (rdma_req_t *)sr->wr_id;
			if (req && req->msg_buf.len){
				ltgbuf_free(&req->msg_buf);
			}
			sr = sr->next;

                        nr++;
		}

		LTG_ASSERT(node->send_count == nr);
	}

        LTG_ASSERT(corenet);
        LTG_ASSERT(sockid->addr);
        LTG_ASSERT(sockid->type == SOCKID_CORENET);

        __corenet_rdma_free_node(corenet, node);
}

static int __corenet_get_free_node(corenet_node_t array[], int size)
{
        int ret, i;
        corenet_node_t *node;

        for (i = 0; i < size; i++) {
                node = &array[i];

                ret = ltg_spin_lock(&node->lock);
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);

                if (!node->in_use) {
                        node->in_use = 1;
                        ltg_spin_unlock(&node->lock);
                        return i;
                }

                ltg_spin_unlock(&node->lock);
        }

        return -EBUSY;
}

int corenet_rdma_add(core_t *_core, sockid_t *sockid, void *ctx, core_exec exec, core_exec1 exec1,
                     func_t reset, func_t check, func_t recv, rdma_conn_t **_handler)
{
        int ret = 0, event, loc;
        corenet_node_t *node;
        __corenet_t *_corenet = (__corenet_t *)_core->corenet;
        corenet_rdma_t *corenet;
        rdma_conn_t *handler = NULL;
        struct in_addr sin_addr;
        char peer_addr[MAX_NAME_LEN] = "";

        LTG_ASSERT(sockid->addr);
        LTG_ASSERT(sockid->type == SOCKID_CORENET);
        LTG_ASSERT(_corenet->rdma_net);

        corenet = _corenet->rdma_net;

        event = EPOLLIN;

        loc = __corenet_get_free_node(&corenet->array[0], corenet->corenet.count);
        if (loc < 0) {
                ret = -loc;
                GOTO(err_ret, ret);
        }

        node = &corenet->array[loc];

        handler = &node->handler;
        handler->core = _core;
        handler->node_loc = loc;
        handler->is_connected = 0;
        handler->channel = NULL;

        handler->nr_get = 0;
        handler->nr_ack = 0;

        handler->nr_flush = 0;
        handler->nr_other = 0;

        sockid->sd = loc;
        sockid->rdma_handler = handler;

        sin_addr.s_addr = sockid->addr;
        strcpy(peer_addr, inet_ntoa(sin_addr));

        LTG_ASSERT((event & EPOLLOUT) == 0);

        if (check) {
                __corenet_rdma_checklist_add(corenet, node);
        } else {
                INIT_LIST_HEAD(&node->hook);
        }

        node->ev = event;
        node->ctx = ctx;
        node->exec = exec;
        node->exec1 = exec1;
        node->reset = reset;
        node->recv = recv;
        node->check = check;
        node->sockid = *sockid;

        corenet_rdma_get(handler, 1, __FUNCTION__, 1); /*open connection*/

        DINFO("add host sd %d, ev %o:%o, rdma handler:%p cm_id %p\n",
              loc, node->ev, event, handler, handler->cm_id);

        CORENET_RDMA_NODE_DUMP(node);
        RDMA_CONN_DUMP(handler);
        SOCKID_DUMP(sockid);

        *_handler = handler;
        return 0;
err_ret:
        return ret;
}

static void __corenet_rdma_close(rdma_conn_t *rdma_handler)
{
        corenet_node_t *node = NULL;
        __corenet_t *corenet = (__corenet_t *)rdma_handler->core->corenet;
        corenet_rdma_t *__corenet_rdma__ = (corenet_rdma_t *)corenet->rdma_net;
        struct rdma_cm_id *cm_id = rdma_handler->cm_id;
        rdma_info_t *rinfo = rdma_handler->dev;

        LTG_ASSERT( rdma_handler->nr_get == rdma_handler->nr_ack);

        node = &__corenet_rdma__->array[rdma_handler->node_loc];
	struct ibv_send_wr *sr = node->head_sr.next;

        // CMID_DUMP_L(DINFO, cm_id);
        RDMA_CONN_DUMP_L(DINFO, rdma_handler);
        // CORENET_RDMA_NODE_DUMP_L(DINFO, node);

	if (node->send_count > 0) {
		rdma_req_t *req;
		int nr = 0;
		while (sr) {
			req = (rdma_req_t *)sr->wr_id;
			if (req && req->msg_buf.len) {
				ltgbuf_free(&req->msg_buf);
			}
			sr = sr->next;

			nr++;
		}
		LTG_ASSERT(node->send_count == nr);
	}

        IBV_MR_DUMP_L(DINFO, rdma_handler->iov_mr);
	ibv_dereg_mr(rdma_handler->iov_mr);

        ltg_free(&rdma_handler->iov_addr);

        __rdma_destroy_qp(cm_id, __FUNCTION__);

        cm_id->context = NULL;

#if 1
        int ret = rdma_destroy_id(cm_id);
        if (ret == -1) {
                DWARN("cm_id %p ret %d\n", cm_id, errno);
        }

        if (rdma_handler->channel != NULL) {
                rdma_event_del(rdma_handler->channel->fd);

                rdma_destroy_event_channel(rdma_handler->channel);
        }
#endif

        rinfo->nr_conn--;

        if (node->reset)
                node->reset(node->ctx);

        LTG_ASSERT(rdma_handler == &node->handler);

        __corenet_rdma_free_node(__corenet_rdma__, node);
}

inline void S_LTG corenet_rdma_get(rdma_conn_t *rdma_handler, int n,
                                   const char *caller, int verbose)
{
        rdma_handler->ref += n;

#if 0
        if (verbose) {
                RDMA_CONN_DUMP_L3(DINFO, caller, rdma_handler);
        }
#else
        (void) caller;
        (void) verbose;
#endif
}

void S_LTG corenet_rdma_put(rdma_conn_t *rdma_handler, const char *caller,
                            int verbose)
{
        rdma_handler->ref--;

#if 0
        if (unlikely(verbose)) {
                RDMA_CONN_DUMP_L3(DINFO, caller, rdma_handler);
        }
#else
        (void) caller;
        (void) verbose;
#endif

        if (unlikely(rdma_handler->ref == 0)) {
                __corenet_rdma_close(rdma_handler);
        }
}

void corenet_rdma_close(rdma_conn_t *rdma_handler, const char *caller)
{
        int ret;
        corenet_node_t *node = container_of(rdma_handler, corenet_node_t, handler);

        DBUG("sockid %d srv_running %d rdma_running %d\n",
              node->sockid.sd, srv_running, rdma_running);

        if (node->sockid.sd == -1 || srv_running == 0 || rdma_running == 0)
                return;

        if (rdma_handler->is_closing == 0) {
                LTG_ASSERT(rdma_handler->cm_id != NULL);
                ret = rdma_disconnect(rdma_handler->cm_id);
                if (ret == -1) {
                        DWARN("cm_id %p ret %d\n", rdma_handler->cm_id, errno);
                }

                rdma_handler->is_closing = 1;

                list_del_init(&node->send_list);

                corerpc_rdma_reset(&node->sockid);

                RDMA_CONN_DUMP_L3(DINFO, caller, rdma_handler);
                CORENET_RDMA_NODE_DUMP_L(DINFO, node);
        }
}

void __iovs_post_recv_init(rdma_req_t *req, void *ptr)
{
        struct ibv_sge *sge;

        req->mode = RDMA_RECV_MSG;
        req->err = 0;

        sge = &req->sge[0];
        sge->addr = (uintptr_t)ptr;
        sge->length = RDMA_MESSAGE_SIZE;
        sge->lkey = req->rdma_handler->iov_mr->lkey;

        memset(&req->wr.rr, 0, sizeof(struct ibv_recv_wr));
        req->wr.rr.next = NULL;
        req->wr.rr.wr_id = (uint64_t)req;
        req->wr.rr.sg_list = sge;
        req->wr.rr.num_sge = 1;
}

int corenet_rdma_post_recv(void *ptr)
{
        int ret;
        rdma_req_t *req = ptr;
        rdma_conn_t *rdma_handler = NULL;
        struct ibv_recv_wr *bad_wr;

        // check flag, 0 post recv, other close
        if (unlikely(req == NULL || rdma_running == 0 || srv_running == 0))
                return 0;

        rdma_handler = req->rdma_handler;

        if (likely(rdma_handler->is_closing == 0)){

                if (req->mode == RDMA_READ)
                        __iovs_post_recv_init(req, ptr - RDMA_MESSAGE_SIZE);
                else
                        LTG_ASSERT(req->mode == RDMA_RECV_MSG);

                ret = ibv_post_recv(req->rdma_handler->qp, &req->wr.rr, &bad_wr);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        } else {
                corenet_rdma_put(rdma_handler, __FUNCTION__, 1);
        }

        return 0;
err_ret:
        corenet_rdma_put(rdma_handler, __FUNCTION__, 1);
        return ret;
}

rdma_req_t *build_post_send_req(rdma_conn_t *rdma_handler, ltgbuf_t *buf,
                                void **addr, uint32_t rkey, uint32_t size)
{
        struct ibv_send_wr *sr = NULL;
        rdma_req_t *req = NULL;
        void *ptr = ltgbuf_head1(buf, sizeof(ltg_net_head_t));
        ltg_net_head_t *net_head = ptr;

        (void)addr;
        (void)rkey;
        (void)size;

        LTG_ASSERT(net_head->magic == LTG_MSG_MAGIC);

        LTG_ASSERT(sizeof(rdma_req_t) <= RDMA_INFO_SIZE);
        LTG_ASSERT(buf->len <= RDMA_MESSAGE_SIZE);
        req = (rdma_req_t *)(ptr + RDMA_MESSAGE_SIZE);
        ltgbuf_init(&req->msg_buf, 0);

        req->ref = ltgbuf_trans_sge(req->sge, buf, &req->msg_buf, rdma_handler->mr->lkey);
        LTG_ASSERT(req->ref == 1);

        req->mode = RDMA_SEND_MSG;
        req->rdma_handler = rdma_handler;
        sr = &req->wr.sr[0];

        memset(sr, 0x00, sizeof(struct ibv_send_wr));
        sr->next = NULL;
        sr->wr_id = (uint64_t)req;
        sr->sg_list = &req->sge[0];
        sr->num_sge = 1;
        sr->opcode = IBV_WR_SEND;
        sr->send_flags = IBV_SEND_SIGNALED;

        RDMA_REQ_DUMP_L(DBUG, req);
        return req;
}

rdma_req_t S_LTG *build_rdma_read_req(rdma_conn_t *rdma_handler, ltgbuf_t *buf,
                                      void **addr, uint32_t rkey, uint32_t size)
{
        struct ibv_send_wr *sr, *tail, head;
        rdma_req_t *req ;
        void *ptr = ltgbuf_head1(buf, sizeof(ltg_net_head_t));
        int index;
        ltgbuf_t _buf;

        ltg_net_head_t *net_head = ptr;
        LTG_ASSERT(net_head->magic == LTG_MSG_MAGIC);

        LTG_ASSERT(rkey > 0);
        LTG_ASSERT(buf->len <= RDMA_MESSAGE_SIZE);
        LTG_ASSERT(sizeof(rdma_req_t) <= RDMA_INFO_SIZE);

        req = (rdma_req_t *)(ptr + RDMA_MESSAGE_SIZE);

        req->mode = RDMA_READ;
        req->n = 0;
        req->rdma_handler = rdma_handler;

        ltgbuf_init(&req->msg_buf, 0);
        ltgbuf_merge(&req->msg_buf, buf);

        ltgbuf_init(&_buf, size);
        req->ref = ltgbuf_trans_sge(req->sge, NULL, &_buf, rdma_handler->mr->lkey);
        ltgbuf_merge(&req->msg_buf, &_buf);

        LTG_ASSERT(req->ref <= MAX_SGE);

        tail = &head;
        tail->next = NULL;
        for (index = 0; index < req->ref; index++) {
                sr = &req->wr.sr[index];
                memset(sr, 0x00, sizeof(struct ibv_send_wr));
                sr->wr_id = (uint64_t)req;
                sr->sg_list = &req->sge[index];
                sr->num_sge = 1;
                sr->opcode = IBV_WR_RDMA_READ;
                sr->send_flags = IBV_SEND_SIGNALED;
                sr->wr.rdma.remote_addr = (uint64_t)addr[index];
                sr->wr.rdma.rkey = rkey;

                tail->next = sr;
                tail = sr;
                tail->next = NULL;
        }

        RDMA_REQ_DUMP_L(DBUG, req);
        return req;
}

rdma_req_t *build_rdma_write_req(rdma_conn_t *rdma_handler, ltgbuf_t *buf,
                                 void **addr, uint32_t rkey, uint32_t size)
{
        struct ibv_send_wr *sr, head, *tail, *msg_sr;
        rdma_req_t *req = NULL;
        void *ptr = ltgbuf_head1(buf, sizeof(ltg_net_head_t));
        int index;
        (void)size;
        ltg_net_head_t *net_head = ptr;
        LTG_ASSERT(net_head->magic == LTG_MSG_MAGIC);
        req = (rdma_req_t *)(ptr + RDMA_MESSAGE_SIZE);

        req->mode = RDMA_WRITE;
        req->n = 0;
        req->rdma_handler = rdma_handler;

        ltgbuf_init(&req->msg_buf, 0);

        req->ref = ltgbuf_trans_sge(req->sge, buf, &req->msg_buf, rdma_handler->mr->lkey);
        LTG_ASSERT(req->ref <= MAX_SGE);

        msg_sr = &req->wr.sr[req->ref - 1];
        msg_sr->wr_id = (uint64_t)req;
        msg_sr->sg_list = &req->sge[0];
        msg_sr->num_sge = 1;
        msg_sr->opcode = IBV_WR_SEND;
        msg_sr->send_flags = IBV_SEND_SIGNALED;
        msg_sr->next = NULL;

        if (req->ref == 1) {
                return req;
        }

        LTG_ASSERT(rkey > 0);
        head.next = NULL;
        tail = &head;
        for (index = 0; index < req->ref - 1; index++) {
                sr = &req->wr.sr[index];
                memset(sr, 0x00, sizeof(struct ibv_send_wr));
                sr->wr_id = (uint64_t)req;
                sr->sg_list = &req->sge[index + 1];
                //DINFO("rdma write addr %p %d\n", req->sge[index + 1].addr, req->sge[index + 1].length);
                sr->num_sge = 1;
                sr->opcode = IBV_WR_RDMA_WRITE;
                sr->send_flags = IBV_SEND_SIGNALED;
                sr->wr.rdma.remote_addr = (uint64_t)addr[index];
                sr->wr.rdma.rkey = rkey;

                tail->next = sr;
                tail = sr;
        }

        /*send message after RDMA_WRITE*/
        tail->next = msg_sr;

        RDMA_REQ_DUMP_L(DBUG, req);
        return req;
}

/**
 * @param wc
 * @param core
 * @return
 *
 * @see corerpc_rdma_recv_msg
 * @see __corenet_rdma_add
 */
static int S_LTG __corenet_rdma_handle_wc(struct ibv_wc *wc, __corenet_t *corenet)
{
        rdma_conn_t *rdma_handler;
        corenet_rdma_t *__corenet_rdma__ = corenet->rdma_net;
        corenet_node_t *node;
        rdma_req_t *req;
        int count = 0;

        req = (rdma_req_t *)wc->wr_id;
        rdma_handler = req->rdma_handler;
        node = &__corenet_rdma__->array[rdma_handler->node_loc];

        rdma_handler->nr_success++;

        RDMA_REQ_DUMP_L(DBUG, req);

        switch (req->mode) {
        case RDMA_RECV_MSG:
                count = wc->byte_len;
                node->exec(node->ctx, (void *)req, &count);
                break;
        case RDMA_SEND_MSG:
                req->ref--;
                LTG_ASSERT(req->ref == 0);
                ltgbuf_free(&req->msg_buf);
                corenet_rdma_put(rdma_handler, __FUNCTION__, 0);
                break;
        case RDMA_READ:
                req->ref--;
                if (req->ref)
                     break;

                node->exec1(node->ctx, &req->msg_buf);
                LTG_ASSERT(req->msg_buf.len == 0);
                break;
        case RDMA_WRITE:
                req->ref--;
                if (req->ref == 0) {
                        ltgbuf_free(&req->msg_buf);
                }
                corenet_rdma_put(rdma_handler, __FUNCTION__, 0);
                break; /*do nothing*/
        default:
                DERROR("bad mode:%d\n", req->mode);
                LTG_ASSERT(0);
        }

        //DINFO("rdma poll(%p %p) ref %d\n", req, rdma_handler, rdma_handler->ref);

        return 0;
}

STATIC int __corenet_rdma_handle_wc_error(struct ibv_wc *wc, __corenet_t *corenet)
{
        rdma_req_t *req;
        rdma_conn_t *rdma_handler;
        struct rdma_cm_id *cm_id;

        //   void *ptr;
        req = (rdma_req_t *)wc->wr_id;
        rdma_handler = req->rdma_handler;
        cm_id = rdma_handler->cm_id;

        corenet_rdma_t *__corenet_rdma__ = corenet->rdma_net;
        corenet_node_t *node = &__corenet_rdma__->array[rdma_handler->node_loc];

        DBUG("rdma_conn %p %ju/%ju/%ju cmid %p qp %p mode %d status %d opcode %d\n",
              rdma_handler,
              rdma_handler->nr_success,
              rdma_handler->nr_flush,
              rdma_handler->nr_other,
              cm_id, cm_id->qp,
              req->mode, wc->status, wc->opcode);

        if (wc->status == IBV_WC_LOC_PROT_ERR) {
                CORENET_RDMA_NODE_DUMP_L(DERROR, node);
                RDMA_REQ_DUMP_L(DERROR, req);

                RDMA_CONN_DUMP_L(DERROR, rdma_handler);

                if (rdma_handler->core) {
                        CORE_DUMP_L(DERROR, rdma_handler->core);
                }

                CMID_DUMP_L(DERROR, cm_id);
                IBV_QP_DUMP_L(DERROR, cm_id->qp);

                IBV_MR_DUMP_L(DERROR, rdma_handler->mr);
                IBV_MR_DUMP_L(DERROR, rdma_handler->iov_mr);

                LTG_ASSERT(0);
        } else if (wc->status == IBV_WC_WR_FLUSH_ERR){
                rdma_handler->nr_flush++;
        } else {
                DWARN("poll error!!!!!! wc status:%s(%d), CQ:%p\n",
                      ibv_wc_status_str(wc->status), (int)wc->status, rdma_handler);
                rdma_handler->nr_other++;
        }

        switch (req->mode) {
        case RDMA_RECV_MSG:
                break;
        case RDMA_SEND_MSG:
                ltgbuf_free(&req->msg_buf);
                break;
        case RDMA_READ:
                ltgbuf_free(&req->msg_buf);
                break;
        case RDMA_WRITE:
                ltgbuf_free(&req->msg_buf);
                break;
        default:
                DERROR("bad mode:%d\n", req->mode);
                LTG_ASSERT(0);
        }

        corenet_rdma_close(rdma_handler, __FUNCTION__);

        //DINFO("rdma_handler (%p %p) ref %d\n", req, rdma_handler, rdma_handler->ref);
        corenet_rdma_put(rdma_handler, __FUNCTION__, 1);

        return 0;
}

#define MAX_POLLING 32

int S_LTG corenet_rdma_poll(__corenet_t *corenet)
{
        int ret, i, polling_count = 0;
        struct ibv_wc wc[MAX_POLLING];
        rdma_info_t *rinfo;

        if (unlikely(corenet->dev_count == 0 || srv_running == 0 ||rdma_running == 0)) {
                return 0;
        }

#if 0
        for (i = 0; i < corenet->dev_count; i++) {
                // memset(wc, 0x0, sizeof(struct ibv_wc) * MAX_POLLING);

                polling_count = 0;

                rinfo = &corenet->dev_list[i];

                ret = ibv_poll_cq(rinfo->cq, MAX_POLLING, &wc[polling_count]);
                if (unlikely(ret < 0)) {
                        LTG_ASSERT(0);
                }

                polling_count = ret;

                for (int j = 0; j < polling_count; j++) {
                        DBUG("status %d opcode %d len %d\n",
                             wc[j].status, wc[j].opcode, wc[j].byte_len);

                        if (likely(wc[j].status == IBV_WC_SUCCESS)) {
                                __corenet_rdma_handle_wc(&wc[j], corenet);
                        } else {
                                __corenet_rdma_handle_wc_error(&wc[j], corenet);
                        }
                }
        }
#else
	for (i = 0; i < corenet->dev_count; i++) {
                rinfo = &corenet->dev_list[i];

                ret = ibv_poll_cq(rinfo->cq, MAX_POLLING / corenet->dev_count,
                                  &wc[polling_count]);
		if (unlikely(ret < 0)) {
			LTG_ASSERT(0);
		}

                polling_count += ret;
	}

        for (i = 0; i < polling_count; i++) {
                DBUG("status %d opcode %d len %d\n",
                      wc[i].status, wc[i].opcode, wc[i].byte_len);

                if (likely(wc[i].status == IBV_WC_SUCCESS)) {
                        __corenet_rdma_handle_wc(&wc[i], corenet);
                } else {
                        __corenet_rdma_handle_wc_error(&wc[i], corenet);
                }
        }
#endif

        return 0;
}

static void S_LTG __corenet_rdma_queue(corenet_rdma_t *corenet, corenet_node_t *node)
{
	/*struct list_head *pos;
	corenet_node_t *_node;
        int found = 0;*/

        if (list_empty(&node->send_list)) {
              list_add_tail(&node->send_list, &corenet->corenet.forward_list);
        }

	/*list_for_each(pos, &corenet->corenet.forward_list) {
		_node = container_of(pos, corenet_node_t, send_list);
		if (node == _node){
                        found = 1;
			break;
                }
	}

        if (found == 0) */

        return ;
}

int S_LTG corenet_rdma_send(const sockid_t *sockid, ltgbuf_t *buf, void **addr,
                            uint32_t rkey, uint32_t size,
                            rdma_req_t *(*build_req)(rdma_conn_t *rdma_handler, ltgbuf_t *buf,
                                                       void **addr, uint32_t rkey, uint32_t size))
{
        int ret;
        corenet_node_t *node;
        corenet_rdma_t *__corenet_rdma__ = __corenet_get();
        rdma_req_t *req;
        rdma_conn_t *handler;

        LTG_ASSERT(sockid->type == SOCKID_CORENET);

        node = &__corenet_rdma__->array[sockid->sd];
        if (unlikely(node->handler.is_closing == 1 || node->sockid.seq
                     != sockid->seq || node->sockid.sd == -1)) {
                ret = ECONNRESET;
                GOTO(err_lock, ret);
        }

        handler = &node->handler;
        req = build_req(handler, buf, addr, rkey, size);

        node->last_sr->next = &req->wr.sr[0];
        node->last_sr = &req->wr.sr[req->ref - 1];
        node->send_count += req->ref;

	__corenet_rdma_queue(__corenet_rdma__, node);

#if 0
        corenet_rdma_commit(__corenet_rdma__);
#endif

        return 0;
err_lock:
        return ret;
}

static inline int S_LTG __corenet_rdma_commit(corenet_node_t *node)
{
        int ret;
        rdma_conn_t *handler;
        struct ibv_send_wr *bad_wr;
        handler = &node->handler;

        if (rdma_running == 0)
                return 0;

        //DINFO("begin commit count %d req on %p\n", node->send_count, handler);
        ret = ibv_post_send(handler->qp, node->head_sr.next, &bad_wr);
        if (unlikely(ret)) {
                DERROR("ibv_post_send fail, QP_NUM:0x%x, bad_wr:%p, errno:%d, errmsg:%s\n",
                        handler->qp->qp_num, bad_wr, ret, strerror(ret));
                return -1;
        }

        corenet_rdma_get(handler, node->send_count, __FUNCTION__, 0);
        // IBV_QP_DUMP_L(DINFO, handler->qp);

        node->head_sr.next = NULL;
        node->last_sr = &node->head_sr;
        node->send_count = 0;

        return 0;
}

void S_LTG corenet_rdma_commit(void *rdma_net)
{
        struct list_head *pos, *n;
        corenet_rdma_t *__corenet_rdma__ = rdma_net;
        corenet_node_t *node;
        int ret;

        if (unlikely(rdma_net == NULL || srv_running == 0 || rdma_running == 0)) {
                return;
        }

        if (list_empty(&__corenet_rdma__->corenet.forward_list)) {
                return;
        }
        
        list_for_each_safe(pos, n, &__corenet_rdma__->corenet.forward_list) {
                node = container_of(pos, corenet_node_t, send_list);
                LTG_ASSERT(node->in_use == 1);
                ret = __corenet_rdma_commit(node);
                if(unlikely(ret))
                        return

               list_del_init(&node->send_list);
        }
}

int S_LTG corenet_rdma_connected(const sockid_t *sockid)
{
        int ret;
        corenet_node_t *node;

        if (sockid->sd == -1) {
                SOCKID_DUMP_L(DWARN, sockid);
                return 0;
        }

        corenet_rdma_t *__corenet_rdma__ = __corenet_get();

        node = &__corenet_rdma__->array[sockid->sd];

        if (unlikely(node->sockid.seq != sockid->seq || node->sockid.sd == -1
                     || !node->handler.is_connected || node->handler.is_closing == 1
                     || node->in_use == 0)) {
                SOCKID_DUMP(sockid);
                SOCKID_DUMP(&node->sockid);

                CORENET_RDMA_NODE_DUMP(node);
                RDMA_CONN_DUMP(&node->handler);

                ret = ECONNRESET;
                // DWARN("for bug test the node sockid is %d sockid seq is %d\n",node->sockid.sd,sockid->seq);
                GOTO(err_lock, ret);
        }

        LTG_ASSERT(node->in_use == 1);

        return 1;
err_lock:
        return 0;
}

int corenet_rdma_init(int max, corenet_rdma_t **_corenet)
{
        int ret, len, i, size;
        corenet_rdma_t *corenet;
        corenet_node_t *node;

        size = max;

        DINFO("rdma malloc max %d size %ju\n", max, sizeof(corenet_node_t) * size);

        len = sizeof(corenet_rdma_t) + sizeof(corenet_node_t) * size;
        ltg_malloc((void **)&corenet, sizeof(corenet_rdma_t) + max * sizeof(corenet_node_t));

        memset(corenet, 0x0, len);
        corenet->corenet.count = size;

        for (i = 0; i < size; i++) {
                node = &corenet->array[i];

                node->sockid.sd = -1;
                node->in_use = 0;

                node->head_sr.next = NULL;
                node->last_sr = &node->head_sr;

                ltg_spin_init(&node->lock);
                INIT_LIST_HEAD(&node->send_list);
        }

        INIT_LIST_HEAD(&corenet->corenet.forward_list);
        INIT_LIST_HEAD(&corenet->corenet.check_list);

        ret = ltg_spin_init(&corenet->corenet.lock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        core_tls_set(VARIABLE_CORENET_RDMA, corenet);

        if (_corenet)
                *_corenet = corenet;

        DBUG("corenet init done\n");

        return 0;

err_ret:
        return ret;
}

/*********************************************************/
int corenet_rdma_evt_channel_init()
{
        int ret;
        uint64_t size = sizeof(struct rdma_event_channel *) * CORE_MAX;

        ret = ltg_malloc((void **)&corenet_rdma_evt_channel, size);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

struct rdma_event_channel *corenet_rdma_get_evt_channel(int cpu_idx)
{
        return corenet_rdma_evt_channel[cpu_idx];
}

int corenet_rdma_create_channel(int cpu_idx)
{
        int ret;

        corenet_rdma_evt_channel[cpu_idx] = rdma_create_event_channel();
        if (!corenet_rdma_evt_channel[cpu_idx]) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

int corenet_rdma_get_event(int cpu_idx, struct rdma_cm_event **ev)
{
        int ret;

        ret = rdma_get_cm_event(corenet_rdma_get_evt_channel(cpu_idx), ev);
        if (ret < 0) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

int __rdma_create_cq(rdma_info_t **_dev, struct rdma_cm_id *cm_id, int ib_port, __corenet_t *corenet)
{
        int cq_size = 0;
        int ret = 0;
        struct ibv_port_attr port_attr;
        struct ibv_device_attr device_attr;
        rdma_info_t *dev;
        void *private_mem;
        uint64_t private_mem_size = 0;

        CMID_DUMP_L(DINFO, cm_id);

        LTG_ASSERT(corenet->dev_count < MAX_RDMA_DEV);
        dev = &corenet->dev_list[corenet->dev_count];

        dev->ibv_verbs = cm_id->verbs;

        dev->nr_conn = 0;

        dev->nr_success = 0;
        dev->nr_flush = 0;
        dev->nr_other = 0;

        /* query port properties */
        if (ibv_query_port(cm_id->verbs, ib_port, &port_attr)) {
                DERROR("ibv_query_port on port %u failed, errno:%d, errmsg:%s\n",
                        ib_port, errno, strerror(errno));
                ret = errno;
                GOTO(err_free, ret);
        }

        /* query device properties */
        ret = ibv_query_device(dev->ibv_verbs, &device_attr);
        if (ret < 0) {
                ret = -ret;
                GOTO(err_ret, ret);
        }

        /*a bug fix, 512k may be too large*/
        cq_size = min_t(uint32_t, device_attr.max_cqe, MAX_POLL_CQ_SIZE);

        /* each side will send only one WR, so Completion
         * Queue with 1 entry is enough
         */
        dev->cq = ibv_create_cq(cm_id->verbs, cq_size, NULL, NULL, 0);
        if (!dev->cq) {
                DERROR("failed to create CQ with %u entries, errno:%d, errmsg:%s\n",
                       cq_size, errno, strerror(errno));
                ret = errno;
                GOTO(err_free, ret);
        }

        DINFO("max %d CQEs cq %p\n", cq_size, dev->cq);
        IBV_CQ_DUMP_L(DINFO, dev->cq);

        dev->pd = ibv_alloc_pd(dev->ibv_verbs);
        if (dev->pd == NULL) {
                LTG_ASSERT(0);
        }

#if 1
        // TODO by core?
        get_global_private_mem(&private_mem, &private_mem_size);

        DINFO("private_mem %p size %jd dev_count %d\n",
              private_mem, private_mem_size, corenet->dev_count);

        dev->mr = (struct ibv_mr *)rdma_register_mr(dev->pd, private_mem, private_mem_size);
        if (dev->mr == NULL)
                LTG_ASSERT(0);
#endif

        corenet->dev_count++;

        //gmr = dev->mr;
        DINFO("CQ was created cq %p pd %p mr %p dev_count %d\n",
              dev->cq, dev->pd, dev->mr, corenet->dev_count);

        RDMA_INFO_DUMP_L(DINFO, dev);

        *_dev = dev;

        return 0;
err_free:
        ltg_free((void **)&dev);
err_ret:
        return ret;
}

void *rdma_register_mr(void *pd, void *buf, size_t size)
{
        int mr_flags = 0;
        struct ibv_mr *mr = NULL;

        /* register the memory buffer */
        mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
        // huge_mem_init here?
        mr = ibv_reg_mr((struct ibv_pd *)pd, buf, size, mr_flags);
        if (!mr) {
                DERROR("ibv_reg_mr failed with mr_flags=0x%x, errno:%d, errmsg:%s %p\n",
                       mr_flags, errno, strerror(errno), buf);
                goto err_ret;
        }

        IBV_MR_DUMP_L(DINFO, mr);
        return mr;
err_ret:
        return NULL;
}

static int __rdma_dev_find(rdma_info_t **_dev, core_t *core, struct rdma_cm_id *cm_id)
{
        rdma_info_t *dev;
        int i;
        __corenet_t *corenet = (__corenet_t *)core->corenet;

        for (i = 0; i < corenet->dev_count; i++) {
                dev = &corenet->dev_list[i];
                if (dev->ibv_verbs == cm_id->verbs) {
                        *_dev = dev;
                        return 1;
                }
        }

        return 0;
}

static int __corenet_rdma_create_qp__(struct rdma_cm_id *cm_id, core_t *core, rdma_conn_t *handler)
{
        int ret;
        struct ibv_qp_init_attr qp_init_attr;
        rdma_info_t *dev;
        __corenet_t *corenet = (__corenet_t *)core->corenet;

        if (!__rdma_dev_find(&dev, core, cm_id)) {
                ret = __rdma_create_cq(&dev, cm_id, 1, corenet);
                if (ret) {
                        DERROR("rdma create cq fail, errno:%d, errmsg:%s\n",
                               errno, strerror(errno));
                        GOTO(err_ret, ret);
                }

                RDMA_INFO_DUMP_L(DINFO, dev);
        }

        LTG_ASSERT(dev->cq != NULL && dev->pd != NULL && dev->mr != NULL);

        /* create qp */
        memset(&qp_init_attr, 0, sizeof(qp_init_attr));
        /* both send and recv to the same CQ */
        qp_init_attr.send_cq = dev->cq;
        qp_init_attr.recv_cq = dev->cq;
        qp_init_attr.cap.max_send_wr = 1024;
        qp_init_attr.cap.max_recv_wr = 1024;
        qp_init_attr.cap.max_send_sge = MAX_SEG_COUNT; /* scatter/gather entries */
        qp_init_attr.cap.max_recv_sge = MAX_SEG_COUNT;
        qp_init_attr.qp_type = IBV_QPT_RC;
        /* only generate completion queue entries if requested */
        qp_init_attr.sq_sig_all = 0;

        DINFO("cap max_wr %d/%d max_sge %d/%d\n",
              1024, 1024,
              qp_init_attr.cap.max_send_sge, qp_init_attr.cap.max_recv_sge);

        if (rdma_running == 0 || srv_running == 0) {
                ret = EIO;
                GOTO(err_ret, ret);
        }

        ret = rdma_create_qp(cm_id, dev->pd, &qp_init_attr);
        if (ret) {
                DERROR("cm_id:%p, ret %d, errno:%d\n", cm_id, ret, errno);
                ret = errno;
                LTG_ASSERT(0);
                GOTO(err_ret, ret);
        }

        handler->cm_id = cm_id;
        handler->cq = dev->cq;
        handler->pd = dev->pd;
        handler->mr = dev->mr;
        handler->qp = cm_id->qp;
        handler->dev = dev;
        handler->core = core;

        dev->nr_conn++;

        return 0;
err_ret:
        return ret;
}

static int __corenet_rdma_post_mem_handler(rdma_conn_t *handler, core_t *core)
{
        int ret, i;
        rdma_req_t *req;
        struct ibv_recv_wr *bad_wr;
        void *ptr, *tmp = NULL;
        uint32_t size = RDMA_INFO_SIZE + RDMA_MESSAGE_SIZE;

        ret = posix_memalign(&tmp, 4096, size * DEFAULT_MH_NUM);
        if (ret) {
                LTG_ASSERT(0);
        }

        if (core->main_core) {
                long unsigned int node_id;
                node_id = core->main_core->node_id;
                mbind(tmp, size, MPOL_PREFERRED, &node_id, 3, 0);
        }

        memset(tmp, 0x0, size * DEFAULT_MH_NUM);

        handler->iov_addr = tmp;
        DINFO("new rdma conn %p\n", handler->iov_addr);

        RDMA_CONN_DUMP(handler);

        LTG_ASSERT(handler->pd != NULL);

        handler->iov_mr = (struct ibv_mr *)rdma_register_mr(handler->pd,
                                                             tmp, size * DEFAULT_MH_NUM);
        if (handler->iov_mr == NULL)
                LTG_ASSERT(0);

        for (i = 0; i < DEFAULT_MH_NUM; i++) {

                static_assert(sizeof(rdma_req_t) <= RDMA_INFO_SIZE, "rdma_req_t");

                ptr = tmp + i * size;

                req = (rdma_req_t *)(ptr + RDMA_MESSAGE_SIZE);
                req->rdma_handler = handler;
                //DINFO("add req %p to rdma_handler %p\n", req, handler);
                __iovs_post_recv_init(req, ptr);

                ret = ibv_post_recv(handler->qp, &req->wr.rr, &bad_wr);
                if (ret)
                        LTG_ASSERT(0);
        }

        handler->is_connected = 1;

        // multiply 2, means both mem_handler and post recv ref
        corenet_rdma_get(handler, DEFAULT_MH_NUM, __FUNCTION__, 1);

        return 0;
}

static int __corenet_rdma_create_qp_real(core_t *core, struct rdma_cm_id *cm_id, rdma_conn_t *rdma_handler)
{
        int ret;

        ret = __corenet_rdma_create_qp__(cm_id, core, rdma_handler);
        if (ret)
                GOTO(err_ret, ret);

        // rdma_handler->core = core;
        // rdma_handler->cm_id = cm_id;
        // rdma_handler->qp = cm_id->qp;
        // rdma_handler->pd = cm_id->pd;

        ret = __corenet_rdma_post_mem_handler(rdma_handler, core);
        if (ret)
                GOTO(err_free, ret);

        RDMA_CONN_DUMP_L(DINFO, rdma_handler);

        return 0;
err_free:
        __rdma_destroy_qp(cm_id, __FUNCTION__);
err_ret:
        return ret;
}

static int __corenet_rdma_create_qp_coroutine(va_list ap)
{
        rdma_conn_t *rdma_handler = va_arg(ap, rdma_conn_t *);
        struct rdma_cm_id *cm_id = va_arg(ap, struct rdma_cm_id *);
        core_t *core = va_arg(ap, core_t *);

        va_end(ap);

        LTG_ASSERT(rdma_handler->ref <= 1);
        if (rdma_handler->ref > 1) {
                DWARN("rdma_handler ref %d\n", rdma_handler->ref);
        }

        return __corenet_rdma_create_qp_real(core, cm_id, rdma_handler);
}

static int __corenet_rdma_create_qp(core_t *core, struct rdma_cm_id *cm_id, rdma_conn_t *rdma_handler)
{
        int ret;

        if (core_self()) {
                return __corenet_rdma_create_qp_real(core, cm_id, rdma_handler);
        } else {

        retry:
                ret = core_request(core->hash, -1, "rdma_create_qp",
                                   __corenet_rdma_create_qp_coroutine,
                                   rdma_handler, cm_id, core);
                if (ret) {
                        if (ret == ENOSPC) {
                                DWARN("core request queue is full, sleep 5ms will retry\n");
                                usleep(5000);
                                goto retry;
                        } else {
                                DWARN("ret %d\n", ret);
                                UNIMPLEMENTED(__DUMP__);
                        }
                }
        }
        return 0;
}

static int __corenet_rdma_add_coroutine(va_list ap)
{
        core_t *core = va_arg(ap, core_t *);
        sockid_t *sockid = va_arg(ap, sockid_t *);
        void *ctx = va_arg(ap, void *);
        core_exec exec = va_arg(ap, core_exec);
        core_exec1 exec1 = va_arg(ap, core_exec1);
        func_t reset = va_arg(ap, func_t);
        func_t check = va_arg(ap, func_t);
        func_t recv = va_arg(ap, func_t);
        rdma_conn_t **handler = va_arg(ap, rdma_conn_t **);

        va_end(ap);

        return corenet_rdma_add(core, sockid, ctx, exec, exec1, reset, check, recv, handler);
}

static int __corenet_rdma_add(core_t *core, sockid_t *sockid, void *ctx, core_exec exec,
                              core_exec1 exec1, func_t reset, func_t check, func_t recv,
                              rdma_conn_t **handler)
{
        int ret;

        if (core_self()) {
                return corenet_rdma_add(core, sockid, ctx, exec, exec1, reset, check, recv, handler);
        } else {
                ret = core_request(core->hash, -1, "rdma_add", __corenet_rdma_add_coroutine,
                                core, sockid, ctx, exec, exec1, reset, check, recv, handler);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }
        }

        return 0;
err_ret:
        return ret;
}

#define RESOLVE_TIMEOUT 500

static int __corenet_rdma_resolve_addr(struct rdma_cm_id *cm_id, const uint32_t addr,
                                       const uint32_t port, sockid_t *sockid)
{
        int ret;
        struct sockaddr_in sin;

        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = addr;
        sin.sin_port = htons(port);

        sockid->addr = addr;

        DINFO("connect to server %s port %u\n", inet_ntoa(sin.sin_addr), port);

        ret = rdma_resolve_addr(cm_id, NULL, (struct sockaddr *)&sin, RESOLVE_TIMEOUT);
        if (ret) {
                ret = errno;
                DERROR("rdma_resolve_addr host %u:%u error, errno:%d\n",addr, port, ret);
                GOTO(err_ret, ret);
        }

        CMID_DUMP_L(DINFO, cm_id);

        return 0;
err_ret:
        return ret;
}

STATIC int __corenet_rdma_disconnect(va_list ap)
{
        rdma_conn_t *rdma_handler = va_arg(ap, rdma_conn_t *);
        va_end(ap);

        if (rdma_handler == NULL || rdma_handler->is_closing == 1)
                return 0;

        corenet_rdma_close(rdma_handler, __FUNCTION__);

        return 0;
}

void corenet_rdma_disconnected(struct rdma_cm_event *ev, void *_core)
{
        int ret;
        struct rdma_cm_id *cm_id = ev->id;
        rdma_conn_t *rdma_handler = cm_id->context;
        core_t *core = _core;

        if (rdma_handler == NULL)
                return;

        RDMA_CONN_DUMP_L(DINFO, rdma_handler);

retry:
        ret = core_request(core->hash, -1, "rdma_disconnected",
                           __corenet_rdma_disconnect, rdma_handler);
        if (ret) {
                if (ret == ENOSPC) {
                        DWARN("core request queue is full, sleep 5ms will retry\n");
                        usleep(5000);
                        goto retry;
                } else {
                        UNIMPLEMENTED(__DUMP__);
                }
        }
}

static inline int __corenet_rdma_tw_exit(va_list ap)
{
        rdma_conn_t *rdma_handler = va_arg(ap, rdma_conn_t *);

        corenet_rdma_put(rdma_handler, __FUNCTION__, 1);
        return 0;
}

void corenet_rdma_timewait_exit(struct rdma_cm_event *ev, void *_core)
{
        int ret;
        struct rdma_cm_id *cm_id = ev->id;
        enum rdma_cm_event_type ev_type = ev->event;
        rdma_conn_t *handler = cm_id->context;
        core_t *core = _core;

        // for rdma_destroy_id
        ret = rdma_ack_cm_event(ev);
        if (unlikely(ret))
                DERROR("ack cm event failed, %s\n", rdma_event_str(ev_type));

        LTG_ASSERT(handler != NULL);

        handler->nr_ack++;
        RDMA_CONN_DUMP_L(DINFO, handler);

retry:
        ret = core_request(core->hash, -1, "rdma_timewait",
                           __corenet_rdma_tw_exit, handler);
        if (ret) {
                if (ret == ENOSPC) {
                        DWARN("core request queue is full, sleep 5ms will retry\n");
                        usleep(5000);
                        goto retry;
                } else {
                        UNIMPLEMENTED(__DUMP__);
                }
        }
}

/**
 * client-side
 */
static int __corenet_rdma_resolve_route(struct rdma_cm_id *cm_id, core_t *core, sockid_t *sockid)
{
        int ret;
        rdma_conn_t *rdma_handler;
        corerpc_ctx_t *ctx;

        CMID_DUMP_L(DINFO, cm_id);

        // create rdma_handler, qp, post recv

        sockid->seq = __seq__++;
        sockid->type = SOCKID_CORENET;

        ctx = slab_static_alloc(sizeof(corerpc_ctx_t));
        if (ctx == NULL)
               LTG_ASSERT(0);

        ctx->running = 0;

        ret = __corenet_rdma_add(core, sockid, ctx, corerpc_rdma_recv_msg,
                                 corerpc_rdma_recv_data, corerpc_close,
                                 NULL, NULL, &rdma_handler);
        if (ret)
                UNIMPLEMENTED(__DUMP__);

        ctx->sockid = *sockid;
        ctx->sockid.reply = corerpc_reply_rdma;

        cm_id->context = rdma_handler;
        //rdma_handler->private_mem = core_tls_get(core, VARIABLE_HUGEPAGE);

        ret = __corenet_rdma_create_qp(core, cm_id, rdma_handler);
        if (ret)
                GOTO(err_free, ret);

        RDMA_CONN_DUMP_L(DINFO, rdma_handler);

        ret = rdma_resolve_route(cm_id, RESOLVE_TIMEOUT);
        if (ret) {
                ret = errno;
                DERROR("rdma_resolve_route cm_id:%p error, errno:%d\n", cm_id, ret);
                GOTO(err_free_qp, ret);
        }

        return 0;
err_free_qp:
        __rdma_destroy_qp(cm_id, __FUNCTION__);
err_free:
        slab_static_free((void *)ctx);
        __corenet_rdma_free_node1(core, sockid);
        return ret;
}

static int __corenet_rdma_connect(struct rdma_cm_id *cm_id)
{
        int ret;
        struct rdma_conn_param cm_params;

        memset(&cm_params, 0, sizeof(cm_params));

        cm_params.responder_resources = 16;
        cm_params.initiator_depth = 16;
        cm_params.retry_count = 5;

        DINFO("cm_id:%p route resolved.\n", cm_id);

        ret = rdma_connect(cm_id, &cm_params);
        if (ret) {
                ret = errno;
                DERROR("rdma_connect cm_id:%p error, errno:%d\n", cm_id, ret);
                GOTO(err_ret, ret);
        }

        DINFO("cm_id:%p connect successful.\n", cm_id);

        return 0;
err_ret:
        return ret;
}

static int __corenet_rdma_on_active_event(struct rdma_event_channel *evt_channel,
                                          core_t *core, sockid_t *sockid)
{
        int ret;
        struct rdma_cm_event *ev = NULL;
        enum rdma_cm_event_type ev_type;

        DINFO("rdma_get_cm_event\n");
        /**
         * rdma_get_cm_event will blocked, so cannot exec in core/task.
         */
        while (1) {
                struct pollfd ev_pollfd;
                int ms_timeout = 20 * 1000;

                /*
                 * poll the channel until it has an event and sleep ms_timeout
                 * milliseconds between any iteration
                 */
                ev_pollfd.fd      = evt_channel->fd;
                ev_pollfd.events  = POLLIN;
                ev_pollfd.revents = 0;

                ret = poll(&ev_pollfd, 1, ms_timeout);
                if (ret < 0) {
                        DERROR("rdma_get_cm_event poll failed, err:%d\r\n", ret);
                        ret = -errno;
                        GOTO(err_ret, ret);
                } else if(ret == 0) {
                        DERROR("rdma_get_cm_event timeout, err:%d\r\n", ret);
                        ret = -errno;
                        GOTO(err_ret, ret);
                }

                ret = rdma_get_cm_event(evt_channel, &ev);
                if(ret) {
                        DERROR("rdma_get_cm_event failed, err:%d\r\n", -errno);
                        ret = -errno;
                        GOTO(err_ret, ret);
                }

                CMID_DUMP_L(DINFO, ev->id);

                ev_type = ev->event;

                switch (ev_type) {
                case RDMA_CM_EVENT_ADDR_RESOLVED:
                        ret = __corenet_rdma_resolve_route(ev->id, core, sockid);
                        if (ret) {
                                GOTO(err_ack, ret);
                        }
                        break;
                case RDMA_CM_EVENT_ROUTE_RESOLVED:
                        ret = __corenet_rdma_connect(ev->id);
                        if (ret) {
                                GOTO(err_ack, ret);
                        }
                        break;
                case RDMA_CM_EVENT_ESTABLISHED:
                        DINFO("connection established on active side. channel:%p\n", evt_channel);
                        rdma_ack_cm_event(ev);
                        goto out;
                case RDMA_CM_EVENT_ADDR_ERROR:
                case RDMA_CM_EVENT_ROUTE_ERROR:
                case RDMA_CM_EVENT_UNREACHABLE:
                case RDMA_CM_EVENT_CONNECT_ERROR:
                        ret = ECONNREFUSED;
                        GOTO(err_ack, ret);
                default:
                        DERROR("Illegal event:%d - ignored\n", ev_type);
                        ret = ECONNREFUSED;
                        GOTO(err_ack, ret);
                }

                rdma_ack_cm_event(ev);
        }

out:
        DINFO("rdma_get_cm_event finish\n");
        return 0;
err_ack:
        rdma_ack_cm_event(ev);
err_ret:
        DERROR("rdma_get_cm_event failed, ret %d\n", ret);
        return ret;
}

#if CORENET_RDMA_ON_ACTIVE_WAIT
static void *__corenet_rdma_on_active_wait__(void *arg)
{
        struct rdma_cm_event *ev = NULL;
        enum rdma_cm_event_type ev_type;
        struct rdma_event_channel *evt_channel = arg;
        rdma_conn_t *rdma_handler = NULL;

        while (rdma_get_cm_event(evt_channel, &ev) == 0) {
                rdma_handler = ev->id->context;
                ev_type = ev->event;

                switch (ev_type) {
                case RDMA_CM_EVENT_CONNECT_ERROR:
                case RDMA_CM_EVENT_ADDR_CHANGE:
                case RDMA_CM_EVENT_DISCONNECTED:
                        DWARN("disconnect on active side. channel:%p, event:%s\n", evt_channel, rdma_event_str(ev_type));
                        corenet_rdma_disconnected(ev, rdma_handler->core);
                        break;
                case RDMA_CM_EVENT_TIMEWAIT_EXIT:
                        DWARN("disconnect on active side. channel:%p, event:%s\n", evt_channel, rdma_event_str(ev_type));
                        corenet_rdma_timewait_exit(ev, rdma_handler->core);
                        rdma_ack_cm_event(ev);
                        goto out;
                default:
                        DERROR("Illegal event:%d - ignored\n", ev_type);
                        break;
                }

                rdma_ack_cm_event(ev);
        }
out:
        return NULL;
}

static int __corenet_rdma_on_active_wait(struct rdma_event_channel *evt_channel)
{
        int ret;

        pthread_t th;
        pthread_attr_t ta;

        (void)pthread_attr_init(&ta);
        (void)pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);

        ret = pthread_create(&th, &ta, __corenet_rdma_on_active_wait__, evt_channel);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}
#endif /*CORENET_RDMA_ON_ACTIVE_WAIT*/

int corenet_rdma_connect_by_channel(const uint32_t addr, const uint32_t port,
                                    core_t *core, sockid_t *sockid)
{
        int ret = 0;
        int flags;
        struct rdma_cm_id *cma_conn_id;
        struct rdma_event_channel *evt_channel;

        DBUG("connect %s:%d\n", _inet_ntoa(addr), port);
        
        ANALYSIS_BEGIN(0);

        //evt_channel = corenet_rdma_get_evt_channel(core->hash);
        evt_channel = rdma_create_event_channel();
        if (!evt_channel) {
                DERROR("rdma create channel fail, errno:%d\n", errno);
                GOTO(err_ret, ret);
        }

        ret = rdma_create_id(evt_channel, &cma_conn_id, NULL, RDMA_PS_TCP);
        if (ret) {
                DERROR("rdma_create_id failed, %m\n");
                ret = errno;
                GOTO(err_ret, ret);
        }

        //CMID_DUMP_L(DINFO, cma_conn_id);

        ret = __corenet_rdma_resolve_addr(cma_conn_id, addr, port, sockid);
        if (ret)
                GOTO(err_ret, ret);

        /* change the blocking mode of the completion channel */
        flags = fcntl(evt_channel->fd, F_GETFL);
        ret = fcntl(evt_channel->fd, F_SETFL, flags | O_NONBLOCK);
        if (ret < 0) {
                DERROR("failed to change file descriptor of Completion Event Channel\n");
                GOTO(err_ret, ret);
        }

        ret = __corenet_rdma_on_active_event(evt_channel, core, sockid);
        if (ret)
                GOTO(err_ret, ret);

#if CORENET_RDMA_ON_ACTIVE_WAIT
        // create a new thread to wait the channel disconnect
        ret = __corenet_rdma_on_active_wait(evt_channel);
        if (ret)
                GOTO(err_ret, ret);
#else
        ret = rdma_event_add(evt_channel->fd, RDMA_CLIENT_EV_FD, EPOLLIN,
                             rdma_handle_event, NULL, core);
        if (ret)
                GOTO(err_ret, ret);
#endif /*CORENET_RDMA_ON_ACTIVE_WAIT*/

        rdma_conn_t *rconn = cma_conn_id->context;
        LTG_ASSERT(rconn != NULL);

        rconn->channel = evt_channel;
        rconn->type = RDMA_CLIENT_EV_FD;

        CMID_DUMP_L(DINFO, cma_conn_id);
        RDMA_CONN_DUMP_L(DINFO, rconn);

        ANALYSIS_END(0, 1000 * 1000 * 5, NULL);
        return 0;

/*err_id:
        rdma_destroy_id(cma_conn_id); */
err_ret:
        ANALYSIS_END(0, 1000 * 1000 * 5, NULL);
        return ret;
}

static int __corenet_rdma_bind_addr(struct rdma_cm_id *cm_id, uint32_t port)
{
        int listen_port = 0, ret = 0;
        struct sockaddr_in sock_addr;

        DINFO("cm_id %p listen port %d\n", cm_id, port);

        listen_port = port;

        memset(&sock_addr, 0, sizeof(sock_addr));
        sock_addr.sin_family = AF_INET;
        sock_addr.sin_port = htons(listen_port);
        sock_addr.sin_addr.s_addr = INADDR_ANY;

        ret = rdma_bind_addr(cm_id, (struct sockaddr *)&sock_addr);
        if (ret) {
                ret = errno;
                DERROR("rdma_bind_addr: %s fail. errno:%d\n", strerror(ret), ret);
                GOTO(err_ret, ret);
        }

        CMID_DUMP_L(DINFO, cm_id);

        return 0;
err_ret:
        return ret;
}

int corenet_rdma_listen_by_channel(int cpu_idx, uint32_t port)
{
        int afonly = 1, ret;
        struct rdma_cm_id *cma_listen_id;
        core_t *core = core_get(cpu_idx);
        struct rdma_event_channel *evt_channel = NULL;

        ret = corenet_rdma_create_channel(cpu_idx);
        if (ret) {
                DERROR("rdma create channel fail, errno:%d\n", ret);
                GOTO(err_ret, ret);
        }

        evt_channel = corenet_rdma_get_evt_channel(cpu_idx);
        ret = rdma_create_id(evt_channel, &cma_listen_id, NULL, RDMA_PS_TCP);
        if (ret) {
                DERROR("rdma_create_id failed, %m\n");
                GOTO(err_ret, ret);
        }

        CMID_DUMP_L(DINFO, cma_listen_id);

        rdma_set_option(cma_listen_id, RDMA_OPTION_ID, RDMA_OPTION_ID_AFONLY, &afonly, sizeof(afonly));

        ret = __corenet_rdma_bind_addr(cma_listen_id, port);
        if (ret) {
                DERROR("rdma bind addr fail : %s\n", strerror(ret));
                ret = EAGAIN;
                GOTO(err_ret, ret);
        }

        /* 0 == maximum backlog */
        ret = rdma_listen(cma_listen_id, 0);
        if (ret) {
                DERROR("rdma_listen fail : %s\n", strerror(ret));
                GOTO(err_ret, ret);
        }

        DINFO("chan %p fd %d\n", evt_channel, evt_channel->fd);
        CMID_DUMP_L(DINFO, cma_listen_id);

#if CORENET_RDMA_ON_ACTIVE_WAIT
        (void)core;
#else
        ret = rdma_event_add(evt_channel->fd, RDMA_SERVER_EV_FD, EPOLLIN, rdma_handle_event, NULL, core);
        if (unlikely(ret))
                GOTO(err_ret, ret);
#endif /*CORENET_RDMA_ON_ACTIVE_WAIT*/

        return 0;
/*err_id:
        rdma_destroy_id(cma_listen_id); */
err_ret:
        return ret;
}

void corenet_rdma_established(struct rdma_cm_event *ev, void *_core)
{
        struct rdma_cm_id *cm_id = ev->id;
        char peer_addr[MAX_NAME_LEN] = "";
        struct sockaddr *addr;
        core_t *core = _core;
        LTG_ASSERT(core->corenet != NULL);

        CMID_DUMP_L(DINFO, cm_id);

        addr = rdma_get_peer_addr(cm_id);
        if (addr == NULL) {
                DERROR("get peer addr fail, maybe disconnect\n");
                return;
        }

        strcpy(peer_addr, inet_ntoa(((struct sockaddr_in *)addr)->sin_addr));

        /*ret = maping_addr2nid(peer_addr, &from);
        if (unlikely(ret)) {
                DERROR("hostname %s trans to nid failret (%u) %s\n", peer_addr, ret, strerror(ret));
                LTG_ASSERT(0);
        }*/

        /*node = &corenet->array[handler->node_loc];
        sockid = &node->sockid;
        sockid->addr = ((struct sockaddr_in *)addr)->sin_addr.s_addr;

        ret = corenet_maping_accept(core, &from, sockid);
        if (ret) {
                UNIMPLEMENTED(__DUMP__);
        }*/

        return;
}

/**
 * server-side
 */
void corenet_rdma_connect_request(struct rdma_cm_event *ev, void *_core)
{
        int ret;
        struct rdma_cm_id *cm_id = ev->id;
        core_t *core = _core;
        rdma_conn_t *rdma_handler;
        sockid_t *sockid;
        corerpc_ctx_t *ctx;

        struct rdma_conn_param conn_param = {
                .responder_resources = 16,
                .initiator_depth = 16,
                .retry_count = 5,
        };

        CMID_DUMP_L(DINFO, cm_id);

        ctx = slab_static_alloc(sizeof(corerpc_ctx_t));
        if (unlikely(ctx == NULL))
                LTG_ASSERT(0);

        ctx->running = 0;

        ctx->sockid.addr = ((struct sockaddr_in *)(&ev->id->route.addr.dst_addr))->sin_addr.s_addr;
        ctx->sockid.seq = __seq__++;
        ctx->sockid.type = SOCKID_CORENET;
        ctx->sockid.reply = corerpc_reply_rdma;

        sockid = &ctx->sockid;

        ret = __corenet_rdma_add(core, sockid, ctx, corerpc_rdma_recv_msg,
                                 corerpc_rdma_recv_data, corerpc_close,
                                 NULL, NULL, &rdma_handler);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        rdma_handler->type = RDMA_SERVER_EV_FD;

        cm_id->context = (void *)rdma_handler;

        ctx->sockid.rdma_handler = (void *)rdma_handler;
        //rdma_handler->private_mem = core_tls_get(core, VARIABLE_HUGEPAGE);

        ret = __corenet_rdma_create_qp(core, cm_id, rdma_handler);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        RDMA_CONN_DUMP_L(DINFO, rdma_handler);

        /* now we can actually accept the connection */
        ret = rdma_accept(cm_id, &conn_param);
        if (unlikely(ret)) {
                DERROR("rdma_accept failed, cm_id:%p\n", cm_id);
                GOTO(err_ret, ret);
        }

        DINFO("corenet rdma accept end, cm_id:%p\n", cm_id);

        return;
err_ret:
        slab_stream_free((void *)ctx);
        ret = rdma_reject(cm_id, NULL, 0);
        if (unlikely(ret))
                DERROR("cm_id:%p rdma_reject failed, %m\n", cm_id);
}

#if CORENET_RDMA_ON_ACTIVE_WAIT

int corenet_rdma_on_passive_event(int cpu_idx)
{
        struct rdma_cm_event *ev = NULL;
        enum rdma_cm_event_type ev_type;
        core_t *core = core_get(cpu_idx);


        while (rdma_get_cm_event(corenet_rdma_get_evt_channel(cpu_idx), &ev) == 0) {
                ev_type = ev->event;
                switch (ev_type) {
                case RDMA_CM_EVENT_CONNECT_REQUEST:
                        corenet_rdma_connect_request(ev, core);
                        break;

                case RDMA_CM_EVENT_ESTABLISHED:
                        DINFO("corenet rdma  connection established on passive side. channel:%p\n", corenet_rdma_get_evt_channel(cpu_idx));
                        corenet_rdma_established(ev, core);
                        break;

                case RDMA_CM_EVENT_REJECTED:
                case RDMA_CM_EVENT_ADDR_CHANGE:
                case RDMA_CM_EVENT_DISCONNECTED:
                        DWARN("disconnect on passive side. channel:%p, event:%s\n", corenet_rdma_get_evt_channel(cpu_idx), rdma_event_str(ev_type));
                        corenet_rdma_disconnected(ev, core);
                        break;

                case RDMA_CM_EVENT_TIMEWAIT_EXIT:
                        corenet_rdma_timewait_exit(ev, core);
                        break;
                default:
                        DERROR("Illegal event:%d - ignored\n", ev_type);
                        break;
                }

                rdma_ack_cm_event(ev);
        }

        return 0;
}

#endif
