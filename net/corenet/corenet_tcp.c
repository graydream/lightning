#include <limits.h>
#include <time.h>
#include <string.h>
#include <sys/epoll.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <netinet/tcp.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_utils.h"
#include "ltg_core.h"
#include "ltg_net.h"
#include "ltg_rpc.h"

extern int ltg_nofile_max;

#define __OP_SEND__ 1
#define __OP_RECV__ 2

typedef corenet_tcp_node_t corenet_node_t;

#if ENABLE_TCP_THREAD

static int __corenet_tcp_wrlock(corenet_node_t *node);
static int __corenet_tcp_rdlock(corenet_node_t *node);
static int __corenet_tcp_rwlock_init(corenet_node_t *node);
static void __corenet_tcp_unlock(corenet_node_t *node);
static int __corenet_tcp_remote(int fd, ltgbuf_t *buf, int op);

#endif

static int __corenet_add(corenet_tcp_t *corenet, const sockid_t *sockid, void *ctx,
                         core_exec exec, func_t reset, func_t check, func_t recv, const char *name);

static void IO_FUNC *__corenet_get()
{
        return core_tls_get(NULL, VARIABLE_CORENET_TCP);
}

static void IO_FUNC *__corenet_get_byctx(void *ctx)
{
        return core_tls_get(ctx, VARIABLE_CORENET_TCP);
}

static void __corenet_set_out(corenet_node_t *node)
{
        int ret, event;
        event_t ev;
        corenet_tcp_t *__corenet__ = __corenet_get();

        if ((node->ev & EPOLLOUT || node->send_buf.len == 0)
            || !(node->ev & EPOLLIN))
                return;

        memset(&ev, 0x0, sizeof(struct epoll_event));

        event = node->ev | EPOLLOUT;
        ev.events = event;
        ev.data.fd = node->sockid.sd;

        DINFO("set sd %u epollfd %u\n", node->sockid.sd, __corenet__->corenet.epoll_fd);

        ret = epoll_ctl(__corenet__->corenet.epoll_fd, EPOLL_CTL_MOD, node->sockid.sd, &ev);
        if (ret == -1) {
                ret = errno;
                LTG_ASSERT(0);
        }

        node->ev = event;
}

static void __corenet_unset_out(corenet_node_t *node)
{
        int ret, event;
        event_t ev;
        corenet_tcp_t *__corenet__ = __corenet_get();

        if (!(node->ev & EPOLLOUT) || node->send_buf.len) {
                return;
        }

        memset(&ev, 0x0, sizeof(struct epoll_event));

        event = node->ev ^ EPOLLOUT;
        ev.data.fd = node->sockid.sd;
        ev.events = event;

        DBUG("unset sd %u epollfd %u\n", ev.data.fd, __corenet__->corenet.epoll_fd);

        ret = epoll_ctl(__corenet__->corenet.epoll_fd, EPOLL_CTL_MOD, node->sockid.sd, &ev);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        node->ev = event;
}

static void __corenet_checklist_add(corenet_tcp_t *corenet, corenet_node_t *node)
{
        int ret;

        ret = ltg_spin_lock(&corenet->corenet.lock);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        list_add_tail(&node->hook, &corenet->corenet.check_list);

        ltg_spin_unlock(&corenet->corenet.lock);
}

static void __corenet_checklist_del(corenet_tcp_t *corenet, corenet_node_t *node)
{
        int ret;

        ret = ltg_spin_lock(&corenet->corenet.lock);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        list_del(&node->hook);

        ltg_spin_unlock(&corenet->corenet.lock);
}


static void __corenet_check_interval()
{
        int ret;
        time_t now;
        struct list_head *pos;
        corenet_node_t *node;
        corenet_tcp_t *__corenet__ = __corenet_get();

        now = gettime();
        if (likely(now - __corenet__->corenet.last_check < 3)) {
                return;
        }

        __corenet__->corenet.last_check  = now;

        DBUG("corenet check\n");

        ret = ltg_spin_lock(&__corenet__->corenet.lock);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        list_for_each(pos, &__corenet__->corenet.check_list) {
                node = (void *)pos;
                node->check(node->ctx);
        }

        ltg_spin_unlock(&__corenet__->corenet.lock);
}

void corenet_tcp_check()
{
        __corenet_check_interval();
}

static int __corenet_add(corenet_tcp_t *corenet, const sockid_t *sockid, void *ctx,
                         core_exec exec, func_t reset, func_t check, func_t recv,
                         const char *name)
{
        int ret, event, sd;
        struct epoll_event ev;
        corenet_node_t *node;
        sche_t *sche = sche_self();

        sd = sockid->sd;
        event = EPOLLIN;
        memset(&ev, 0x0, sizeof(struct epoll_event));

        LTG_ASSERT(sd < ltg_nofile_max);
        node = &corenet->array[sd];

        if (node->ev & event) {
                ret = EEXIST;
                GOTO(err_ret, ret);
        }

        LTG_ASSERT((event & EPOLLOUT) == 0);

        if (check) {
                __corenet_checklist_add(corenet, node);
        } else {
                INIT_LIST_HEAD(&node->hook);
        }

        node->ev = event;
        node->ctx = ctx;
        node->exec = exec;
        node->reset = reset;
        node->recv = recv;
        node->check = check;
        node->sockid = *sockid;

        LTG_ASSERT(node->name == NULL);
        ret = ltg_malloc((void **)&node->name, strlen(name) + 1);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);
        
        strcpy(node->name, name);

        ev.data.fd = sd;
        ev.events = event;
        ret = epoll_ctl(corenet->corenet.epoll_fd, EPOLL_CTL_ADD, sd, &ev);
        if (ret == -1) {
                ret = errno;
                DERROR("%d, %d\n", ret, corenet->corenet.epoll_fd);
                UNIMPLEMENTED(__DUMP__);//remove checklist
        }

        DBUG("corenet_tcp connect %s[%u] %s sd %d, ev %o:%o\n", sche->name,
             sche->id, node->name, sd, node->ev, event);

        return 0;
err_ret:
        return ret;
}

int corenet_tcp_add(corenet_tcp_t *corenet, const sockid_t *sockid, void *ctx,
                    core_exec exec, func_t reset, func_t check,
                    func_t recv, const char *name)
{
        int ret;

        (void) corenet;
        LTG_ASSERT(sockid->type == SOCKID_CORENET);

        ret = __corenet_add(__corenet_get(), sockid, ctx, exec,
                            reset, check, recv, name);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

static int __corenet_tcp_attach(va_list ap)
{
        int ret;
        const sockid_t *sockid = va_arg(ap, const sockid_t *);
        void *ctx = va_arg(ap, void *);
        core_exec exec = va_arg(ap, core_exec);
        func_t reset = va_arg(ap, func_t);
        func_t check = va_arg(ap, func_t);
        func_t recv = va_arg(ap, func_t);
        const char *name = va_arg(ap, const char *);

        va_end(ap);

        ret = __corenet_add(__corenet_get(), sockid, ctx, exec,
                            reset, check, recv, name);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

int corenet_tcp_attach(int coreid, const sockid_t *sockid, void *ctx,
                       core_exec exec, func_t reset, func_t check,
                       func_t recv, const char *name)
{
        int ret;

        LTG_ASSERT(sockid->type == SOCKID_CORENET);

        ret = core_request(coreid, -1, "tcp attach", __corenet_tcp_attach,
                           sockid, ctx, exec, reset, check, recv, name);
        if (ret)
                GOTO(err_ret, ret);
        
        return 0;
err_ret:
        return ret;
}

static void __corenet_close__(const sockid_t *sockid)
{
        int ret, sd;
        event_t ev;
        corenet_tcp_t *__corenet__ = __corenet_get();
        corenet_node_t *node =  &__corenet__->array[sockid->sd];
        sche_t *sche = sche_self();

        LTG_ASSERT(sockid->sd >= 0);
#if ENABLE_TCP_THREAD
        ret = __corenet_tcp_wrlock(node);
        if (unlikely(ret))
                GOTO(err_ret, ret);

#endif
        if (node->sockid.seq != sockid->seq
            || node->sockid.sd == -1) {
                goto out;
        }

        //DBUG("close %d\n", sd);

        sd = node->sockid.sd;
        DBUG("corenet_tcp close %s[%u] %s sd %d, ev %x\n", sche->name,
              sche->id, node->name, sd, node->ev);
        LTG_ASSERT(node->ev);

        if (node->ev) {
                ev.data.fd = sd;
                ev.events = node->ev;
                ret = epoll_ctl(__corenet__->corenet.epoll_fd, EPOLL_CTL_DEL, sd, &ev);
                if (ret == -1) {
                        ret = errno;
                        DERROR("epoll del %d %d\n", sd, node->ev);
                }
        }

        if (node->reset)
                node->reset(node->ctx);

#if 0
        corerpc_reset(&node->sockid);
#endif

        close(node->sockid.sd);
        ltgbuf_free(&node->recv_buf);
        ltgbuf_free(&node->send_buf);

        if (!list_empty(&node->hook)) {
                __corenet_checklist_del(__corenet__, node);
        }

        ltg_free((void **)&node->name);

        node->ev = 0;
        node->ctx = NULL;
        node->exec = NULL;
        node->reset = NULL;
        node->recv = NULL;
        node->sockid.sd = -1;
        node->name = NULL;

out:
#if ENABLE_TCP_THREAD
        __corenet_tcp_unlock(node);
#endif
        return;
#if ENABLE_TCP_THREAD
err_ret:
        return;
#endif
}

static void __corenet_tcp_close_task(void *args)
{
        const sockid_t *sockid = args;
        __corenet_close__(sockid);

        slab_stream_free(args);
}

void corenet_tcp_close(const sockid_t *_sockid)
{
        sockid_t *sockid;

        if (_sockid->sd == -1) {
                return;
        }

        if (sche_running()) {
                __corenet_close__(_sockid);
        } else {
#ifdef HAVE_STATIC_ASSERT
                static_assert(sizeof(*sockid)  < sizeof(mem_cache128_t), "corenet_tcp_close");
#endif

                sockid = slab_stream_alloc(128);
                LTG_ASSERT(sockid);
                *sockid = *_sockid;

                sche_task_new("corenet_tcp_close", __corenet_tcp_close_task, sockid, -1);
                sche_run(NULL);
        }
}

#if !ENABLE_TCP_THREAD
static int __corenet_tcp_local(int fd, ltgbuf_t *buf, int op)
{
        int ret, iov_count;
        struct msghdr msg;
        corenet_tcp_t *__corenet__ = __corenet_get();

        ANALYSIS_BEGIN(0);
        
        iov_count = CORE_IOV_MAX;
        ret = ltgbuf_trans(__corenet__->iov, &iov_count,  buf);
        //LTG_ASSERT(ret == (int)buf->len);
        if(unlikely(ret != (int)buf->len)) {
                DBUG("for bug test tcp send %d, buf->len:%u\n", ret, buf->len);
        }

        memset(&msg, 0x0, sizeof(msg));
        msg.msg_iov = __corenet__->iov;
        msg.msg_iovlen = iov_count;

        if (op == __OP_SEND__) {
                ret = _sendmsg(fd, &msg, MSG_DONTWAIT);
        } else if (op == __OP_RECV__) {
                ret = _recvmsg(fd, &msg, MSG_DONTWAIT);
        } else {
                LTG_ASSERT(0);
        }

        if (ret < 0) {
                ret = -ret;
                DWARN("sd %u %u %s\n", fd, ret, strerror(ret));
                LTG_ASSERT(ret != EMSGSIZE);
                GOTO(err_ret, ret);
        }

        ANALYSIS_QUEUE(0, IO_INFO, NULL);
        
        return ret;
err_ret:
        ANALYSIS_QUEUE(0, IO_INFO, NULL);
        return -ret;
}
#endif

static int __corenet_tcp_recv__(corenet_node_t *node, int toread)
{
        int ret;
        ltgbuf_t buf;

        ANALYSIS_BEGIN(0);

        ret = ltgbuf_init(&buf, toread);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        DBUG("read data %u\n", toread);

#if ENABLE_TCP_THREAD
        ret = __corenet_tcp_remote(node->sockid.sd, &buf, __OP_RECV__);
#else
        ret = __corenet_tcp_local(node->sockid.sd, &buf, __OP_RECV__);
#endif
        if (ret < 0) {
                ret = -ret;
                GOTO(err_free, ret);
        }

        LTG_ASSERT(ret == (int)buf.len);
        ltgbuf_merge(&node->recv_buf, &buf);
        DBUG("new recv %u, left %u\n", buf.len, node->recv_buf.len);

        ANALYSIS_QUEUE(0, IO_INFO, NULL);

        return 0;
err_free:
        ltgbuf_free(&buf);
err_ret:
        return ret;
}

static int __corenet_tcp_recv(corenet_node_t *node, int *count)
{
        int ret, toread;
        uint64_t left, cp;

        ANALYSIS_BEGIN(0);

        ret = ioctl(node->sockid.sd, FIONREAD, &toread);
        if (ret < 0) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        DBUG("recv %u\n", toread);

        if (toread == 0) {
                ret = ECONNRESET;
                GOTO(err_ret, ret);
        }

        left = toread;
        while (left) {
                cp = _min(left, (LLU)BUFFER_SEG_SIZE * CORE_IOV_MAX);
                cp = _min(cp, (1024 * 1024 * 90));

                if ((uint64_t)toread > ((LLU)BUFFER_SEG_SIZE * CORE_IOV_MAX)) {
                        DINFO("long msg, total %u, left %u, read %u\n", toread, left, cp);
                }

                ret = __corenet_tcp_recv__(node, cp);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                left -= cp;
        }

        // __iscsi_newtask_core
        // corerpc_recv
        ret = node->exec(node->ctx, &node->recv_buf, count);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ANALYSIS_QUEUE(0, IO_WARN, NULL);

        return 0;
err_ret:
        return ret;
}

static int __corenet_tcp_send(corenet_node_t *node)
{
        int ret;
        ltgbuf_t *buf;

        ANALYSIS_BEGIN(0);

        buf = &node->send_buf;
        if (likely(buf->len)) {
                DBUG("send %u\n", buf->len);
#if ENABLE_TCP_THREAD
                ret = __corenet_tcp_remote(node->sockid.sd, buf, __OP_SEND__);
#else
                ret = __corenet_tcp_local(node->sockid.sd, buf, __OP_SEND__);
#endif
                if (ret < 0) {
                        ret = -ret;
                        DWARN("forward to %s @ %u len %d fail ret %d\n",
                              _inet_ntoa(node->sockid.addr), node->sockid.sd, buf->len, ret);
                        GOTO(err_ret, ret);
                }

#if 0
                ltgbuf_t tmp;
                ltgbuf_init(&tmp, 0);
                ltgbuf_pop(buf, &tmp, ret);//maybe yield with null arg 2
                ltgbuf_free(&tmp);
#else
                ltgbuf_pop(buf, NULL, ret);//maybe yield with null arg 2
#endif
        }

        ANALYSIS_QUEUE(0, IO_WARN, NULL);

        return 0;
err_ret:
        //ltgbuf_free(buf);
        return ret;
}

#if ENABLE_TCP_THREAD

typedef struct {
        int ev;
        corenet_node_t *node;
} args_t;

typedef struct {
        struct list_head hook;
        task_t task;
        ltgbuf_t *buf;
        int fd;
        int op;
        int retval;
} sr_ctx_t;

typedef struct {
        ltg_spinlock_t lock;
        sem_t sem;
        struct list_head list;
} corenet_tcp_worker_t;

#define __TCP_WORKER_COUNT__ 7

static __thread corenet_tcp_worker_t *__worker__;

static int __corenet_tcp_wrlock(corenet_node_t *node)
{
        int ret;

        LTG_ASSERT(sche_running());
        ret = plock_wrlock(&node->rwlock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (node->sockid.sd == -1) {
                ret = ESTALE;
                GOTO(err_lock, ret);
        }

        return 0;
err_lock:
        plock_unlock(&node->rwlock);
err_ret:
        return ret;
}

inline static int __corenet_tcp_rdlock(corenet_node_t *node)
{
        int ret;

        LTG_ASSERT(sche_running());
        ret = plock_rdlock(&node->rwlock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (node->sockid.sd == -1) {
                ret = ESTALE;
                GOTO(err_lock, ret);
        }

        return 0;
err_lock:
        plock_unlock(&node->rwlock);
err_ret:
        return ret;
}

static int __corenet_tcp_rwlock_init(corenet_node_t *node)
{
        int ret;

        ret = plock_init(&node->rwlock, "corenet_tcp");
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

static void __corenet_tcp_unlock(corenet_node_t *node)
{
        plock_unlock(&node->rwlock);
}

static void __corenet_event_cleanup(corenet_node_t *node, int *_ev)
{
        int ret;
        event_t ev;
        corenet_tcp_t *__corenet__ = __corenet_get();

        LTG_ASSERT(node->ev != 0);

        memset(&ev, 0x0, sizeof(struct epoll_event));

        *_ev = node->ev;
        ev.events = node->ev;
        ev.data.fd = node->sockid.sd;

        DBUG("set sd %u epollfd %u\n", node->sockid.sd, __corenet__->corenet.epoll_fd);

        ret = epoll_ctl(__corenet__->corenet.epoll_fd, EPOLL_CTL_DEL, node->sockid.sd, &ev);
        if (ret == -1) {
                ret = errno;
                LTG_ASSERT(0);
        }

        node->ev = 0;
}

static void __corenet_event_set(corenet_node_t *node, int _ev)
{
        int ret;
        event_t ev;
        corenet_tcp_t *__corenet__ = __corenet_get();

        LTG_ASSERT(node->ev == 0);

        memset(&ev, 0x0, sizeof(struct epoll_event));

        ev.events = _ev;
        ev.events = node->send_buf.len ? (ev.events | EPOLLOUT) : ev.events;
        ev.data.fd = node->sockid.sd;

        DBUG("set sd %u epollfd %u\n", node->sockid.sd, __corenet__->corenet.epoll_fd);

        ret = epoll_ctl(__corenet__->corenet.epoll_fd, EPOLL_CTL_ADD, node->sockid.sd, &ev);
        if (ret == -1) {
                ret = errno;
                LTG_ASSERT(0);
        }

        node->ev = ev.events;
}


static int __corenet_tcp_thread_send(int fd, ltgbuf_t *buf, struct iovec *iov, int iov_count)
{
        int ret;
        struct msghdr msg;

        ANALYSIS_BEGIN(0);

        iov_count = CORE_IOV_MAX / 10;
        ret = ltgbuf_trans(iov, &iov_count, buf);
        //LTG_ASSERT(ret == (int)newbuf.len);
        memset(&msg, 0x0, sizeof(msg));
        msg.msg_iov = iov;
        msg.msg_iovlen = iov_count;

        do {
                struct timeval __tv__;
                gettimeofday(&__tv__, NULL);
                DBUG("begin timeval %u.%llu, buf %u\n", __tv__.tv_sec, (LLU)__tv__.tv_usec, buf->len);
        } while (0);

        ret = _sendmsg(fd, &msg, MSG_DONTWAIT);
        if (ret < 0) {
                ret = -ret;
                DWARN("sd %u %u %s\n", fd, ret, strerror(ret));
                GOTO(err_ret, ret);
        }

        do {
                struct timeval __tv__;
                gettimeofday(&__tv__, NULL);
                DBUG("end timeval %u.%llu, buf %u\n", __tv__.tv_sec, (LLU)__tv__.tv_usec, buf->len);
        } while (0);

        ANALYSIS_QUEUE(0, 10 * 1000, NULL);

        return ret;
err_ret:
        return -ret;
}

static int __corenet_tcp_thread_recv(int fd, ltgbuf_t *buf, struct iovec *iov, int iov_count)
{
        int ret;
        struct msghdr msg;

        ANALYSIS_BEGIN(0);

        ret = ltgbuf_trans(iov, &iov_count,  buf);
        LTG_ASSERT(ret == (int)buf->len);
        memset(&msg, 0x0, sizeof(msg));
        msg.msg_iov = iov;
        msg.msg_iovlen = iov_count;

        do {
                struct timeval __tv__;
                gettimeofday(&__tv__, NULL);
                DBUG("begin timeval %u.%llu, buf %u\n", __tv__.tv_sec, (LLU)__tv__.tv_usec, buf->len);
        } while (0);

        ret = _recvmsg(fd, &msg, MSG_DONTWAIT);
        if (ret < 0) {
                ret = -ret;
                DWARN("sd %u %u %s\n", fd, ret, strerror(ret));
                GOTO(err_ret, ret);
        }

        do {
                struct timeval __tv__;
                gettimeofday(&__tv__, NULL);
                DBUG("end timeval %u.%llu, buf %u\n", __tv__.tv_sec, (LLU)__tv__.tv_usec, buf->len);
        } while (0);

        ANALYSIS_QUEUE(0, 10 * 1000, NULL);

        return ret;
err_ret:
        return -ret;
}


static void *__corenet_tcp_thread(void *args)
{
        int ret, iov_count;
        corenet_tcp_worker_t *worker = args;
        struct list_head list, *pos, *n;
        sr_ctx_t *ctx;
        struct iovec *iov;

        INIT_LIST_HEAD(&list);

        ret = ltg_malloc((void **)&iov, sizeof(*iov) * CORE_IOV_MAX);
        if (ret)
                UNIMPLEMENTED(__DUMP__);

        iov_count = CORE_IOV_MAX;

        while (1) {
                ret = _sem_wait(&worker->sem);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                ret = ltg_spin_lock(&worker->lock);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                list_splice_init(&worker->list, &list);

                ltg_spin_unlock(&worker->lock);

                list_for_each_safe(pos, n, &list) {
                        list_del(pos);
                        ctx = (void *)pos;
                        if (ctx->op == __OP_SEND__) {
                                ctx->retval = __corenet_tcp_thread_send(ctx->fd, ctx->buf,
                                                                        iov, iov_count);
                        } else {
                                ctx->retval = __corenet_tcp_thread_recv(ctx->fd, ctx->buf,
                                                                        iov, iov_count);
                        }
                        sche_task_post(&ctx->task, 0, NULL);
                }
        }

        return NULL;
err_ret:
        UNIMPLEMENTED(__DUMP__);
        return NULL;
}

static int __corenet_tcp_thread_init()
{
        int ret, i;
        corenet_tcp_worker_t *worker, *array;

        LTG_ASSERT(__worker__ == NULL);

        ret = ltg_malloc((void **)&array, sizeof(*array) * __TCP_WORKER_COUNT__);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        for (i = 0; i < __TCP_WORKER_COUNT__; i++) {
                worker = &array[i];
                ret = ltg_spin_init(&worker->lock);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                INIT_LIST_HEAD(&worker->list);

                ret = sem_init(&worker->sem, 0, 0);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                ret = ltg_thread_create(__corenet_tcp_thread, worker, "__tcp_thread_worker");
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }

        __worker__ = array;

        return 0;
err_ret:
        return ret;
}

static int __corenet_tcp_remote(int fd, ltgbuf_t *buf, int op)
{
        int ret;
        sr_ctx_t ctx;
        corenet_tcp_worker_t *worker = &__worker__[fd % __TCP_WORKER_COUNT__];

        ANALYSIS_BEGIN(0);

        ctx.task = sche_task_get();
        ctx.buf = buf;
        ctx.fd = fd;
        ctx.retval = 0;
        ctx.op = op;

        ret = ltg_spin_lock(&worker->lock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        list_add_tail(&ctx.hook, &worker->list);

        ltg_spin_unlock(&worker->lock);

        sem_post(&worker->sem);

        ret = sche_yield1(op == __OP_SEND__ ? "send" : "recv", NULL, NULL, NULL, -1);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        ret = ctx.retval;
        if (unlikely(ret < 0)) {
                ret = -ret;
                GOTO(err_ret, ret);
        }

        ANALYSIS_QUEUE(0, IO_WARN, NULL);

        return ret;
err_ret:
        return -ret;
}

static int __corenet_tcp_exec_request(void *_args)
{
        int ret, count;
        args_t *args = _args;
        corenet_node_t *node = args->node;

        if (args->ev & EPOLLOUT) {
                ret = __corenet_tcp_send(node);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                if (list_empty(&node->send_buf.list)) {
                        __corenet_unset_out(node);
                }
        }

        if (args->ev & EPOLLIN) {
                if (node->recv) {
                        // __core_interrupt_eventfd_func
                        // __core_aio_eventfd_func
                        node->recv(node->ctx);
                } else {
                        // connect sd
                        ret = __corenet_tcp_recv(node, &count);
                        if (unlikely(ret))
                                GOTO(err_ret, ret);
                }
        }

        return 0;
err_ret:
        return ret;
}


static void __corenet_tcp_exec__(void *_args)
{
        int ret, ev;
        args_t *args = _args;
        corenet_node_t *node = args->node;
        sockid_t sockid = node->sockid;

        ret = __corenet_tcp_wrlock(node);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        __corenet_event_cleanup(node, &ev);

        ret = __corenet_tcp_exec_request(_args);
        if (unlikely(ret))
                GOTO(err_lock, ret);

        __corenet_event_set(node, ev);
        __corenet_tcp_unlock(node);

        slab_stream_free(args);

        return;
err_lock:
        __corenet_event_set(node, ev);
        __corenet_tcp_unlock(node);
err_ret:
        slab_stream_free(args);
        corenet_tcp_close(&sockid);
        return;
}

static int __corenet_tcp_exec(void *ctx, corenet_node_t *node, event_t *ev)
{
        int ret;
        args_t *args;
        sockid_t sockid = node->sockid;

        //LTG_ASSERT(node->ev);
        //LTG_ASSERT(node->sockid.sd != -1);

        DBUG("ev %x\n", ev->events);

        if (unlikely((ev->events & EPOLLRDHUP) || (ev->events & EPOLLERR))
            || (ev->events & EPOLLHUP))  {
                ret = ECONNRESET;
                GOTO(err_ret, ret);
        }

#ifdef HAVE_STATIC_ASSERT
        static_assert(sizeof(*args)  < sizeof(mem_cache128_t), "mcache_collect_ctx_t");
#endif
        args = slab_stream_alloc(128);
        args->ev = ev->events;
        args->node = node;

        sche_task_new("corenet_tcp_exec", __corenet_tcp_exec__, args, -1);
        sche_run(core_tls_get(ctx, VARIABLE_SCHEDULE));

        return 0;
err_ret:
        corenet_tcp_close(&sockid);
        return ret;
}

#else

#if 0
static int __corenet_tcp_exec(void *ctx, corenet_node_t *node, event_t *ev)
{
        int ret, recv_msg;

        LTG_ASSERT(node->ev);
        LTG_ASSERT(node->sockid.sd != -1);

        DBUG("ev %x\n", ev->events);

        if (unlikely((ev->events & EPOLLRDHUP) || (ev->events & EPOLLERR))
            || (ev->events & EPOLLHUP))  {
                ret = ECONNRESET;
                GOTO(err_ret, ret);
        }

        if (ev->events & EPOLLOUT) {
                ret = __corenet_tcp_send(node);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                if (list_empty(&node->send_buf.list)) {
                        __corenet_unset_out(node);
                }
        }

        if (ev->events & EPOLLIN) {
                if (node->recv) {
                        // __core_interrupt_eventfd_func
                        // __core_aio_eventfd_func
                        node->recv(node->ctx);
                } else {
                        // connect sd
                        ret = __corenet_tcp_recv(node, &recv_msg);
                        if (unlikely(ret))
                                GOTO(err_ret, ret);
                }
        }

        return 0;
err_ret:
        corenet_tcp_close(&node->sockid);
        return ret;
}

#else

static void __corenet_tcp_exec_recv(void *_node)
{
        int ret, recv_msg;
        corenet_node_t *node = _node;
        sockid_t sockid = node->sockid;

        if (node->recv) {
                // __core_interrupt_eventfd_func
                // __core_aio_eventfd_func
                node->recv(node->ctx);
        } else {
                // connect sd
                ret = __corenet_tcp_recv(node, &recv_msg);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }

        return;
err_ret:
        corenet_tcp_close(&sockid);
        return;

}

static void __corenet_tcp_exec_send(void *_node)
{
        int ret;
        corenet_node_t *node = _node;
        sockid_t sockid = node->sockid;

        ret = __corenet_tcp_send(node);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (list_empty(&node->send_buf.list)) {
                __corenet_unset_out(node);
        }

        return;
err_ret:
        corenet_tcp_close(&sockid);
        return;
}

static int __corenet_tcp_exec(void *ctx, corenet_node_t *node, event_t *ev)
{
        int ret;
        sockid_t sockid = node->sockid;

        if (node->ev == 0 || node->sockid.sd == -1) {
                DERROR("node ev:%d, sockid sd:%d, ev->events:0x%x\n", node->ev, node->sockid.sd, ev->events);
                ret = ESTALE;
                return ret;
        }

        DBUG("ev %x\n", ev->events);

        if (unlikely((ev->events & EPOLLRDHUP) || (ev->events & EPOLLERR))
            || (ev->events & EPOLLHUP))  {
                ret = ECONNRESET;
                GOTO(err_ret, ret);
        }

        if (ev->events & EPOLLOUT) {
                sche_task_new("corenet_tcp_send", __corenet_tcp_exec_send, node, -1);
        }

        if (ev->events & EPOLLIN) {
                sche_task_new("corenet_tcp_recv", __corenet_tcp_exec_recv, node, -1);
        }

        sche_run(core_tls_get(ctx, VARIABLE_SCHEDULE));

        return 0;
err_ret:
        corenet_tcp_close(&sockid);
        return ret;
}

#endif
#endif

int corenet_tcp_poll(void *ctx, int tmo)
{
        int nfds, i;
        event_t events[512], *ev;
        corenet_node_t *node;
        corenet_tcp_t *__corenet__ = __corenet_get_byctx(ctx);

        DBUG("polling %d begin\n", tmo);
        LTG_ASSERT(tmo >= 0 && tmo < ltgconf_global.rpc_timeout * 2);
        nfds = _epoll_wait(__corenet__->corenet.epoll_fd, events, 512, (tmo ? 1 : 0) * 1000);
        if (unlikely(nfds < 0)) {
                UNIMPLEMENTED(__DUMP__);
        }

        DBUG("polling %d return\n", nfds);

        for (i = 0; i < nfds; i++) {
                //ANALYSIS_BEGIN(0);
                ev = &events[i];

                node = &__corenet__->array[ev->data.fd];
                __corenet_tcp_exec(ctx, node, ev);
                //ANALYSIS_QUEUE(0, IO_WARN, "corenet_poll");
        }

        return 0;
}

typedef struct {
        struct list_head hook;
        sockid_t sockid;
        ltgbuf_t buf;
} corenet_fwd_t;

static void __corenet_tcp_queue(corenet_tcp_t *__corenet__, const sockid_t *sockid,
                                ltgbuf_t *buf)
{
        int found = 0;
        corenet_fwd_t *corenet_fwd;
        struct list_head *pos;

        DBUG("corenet_fwd\n");

        list_for_each(pos, &__corenet__->corenet.forward_list) {
                corenet_fwd = (void *)pos;

                if (sockid_cmp(sockid, &corenet_fwd->sockid) == 0) {
                        DBUG("append forward to %s @ %u\n",
                              _inet_ntoa(sockid->addr), sockid->sd);

                        ltgbuf_merge(&corenet_fwd->buf, buf);

                        found = 1;
                        break;
                }
        }

        if (found == 0) {
                DBUG("new forward to %s @ %u\n",
                      _inet_ntoa(sockid->addr), sockid->sd);

                corenet_fwd = slab_stream_alloc(sizeof(*corenet_fwd));
                LTG_ASSERT(corenet_fwd);
                corenet_fwd->sockid = *sockid;
                ltgbuf_init(&corenet_fwd->buf, 0);

                ltgbuf_merge(&corenet_fwd->buf, buf);

                list_add_tail(&corenet_fwd->hook, &__corenet__->corenet.forward_list);
        }
}

int corenet_tcp_send(void *ctx, const sockid_t *sockid, ltgbuf_t *buf)
{
        int ret;
        corenet_node_t *node;
        corenet_tcp_t *__corenet__ = __corenet_get_byctx(ctx);

        ANALYSIS_BEGIN(0);

        LTG_ASSERT(sockid->type == SOCKID_CORENET);
        //LTG_ASSERT(sockid->addr);

        node = &__corenet__->array[sockid->sd];
        if (node->sockid.seq != sockid->seq || node->sockid.sd == -1) {
                ret = ECONNRESET;
                DWARN("seq %d %d, sd %d\n", node->sockid.seq, sockid->seq, node->sockid.sd);
                GOTO(err_ret, ret);
        }

        __corenet_tcp_queue(__corenet__, sockid, buf);

        ANALYSIS_QUEUE(0, 10 * 1000, NULL);

        return 0;
err_ret:
        return ret;
}

#if ENABLE_TCP_THREAD
static int __corenet_tcp_commit(const sockid_t *sockid, ltgbuf_t *buf)
{
        int ret;
        corenet_node_t *node;
        corenet_tcp_t *__corenet__ = __corenet_get();

        LTG_ASSERT(sockid->type == SOCKID_CORENET);
        //LTG_ASSERT(sockid->addr);

        node = &__corenet__->array[sockid->sd];
        ret = __corenet_tcp_wrlock(node);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        if (node->sockid.seq != sockid->seq || node->sockid.sd == -1) {
                ret = ECONNRESET;
                ltgbuf_free(buf);
                GOTO(err_lock, ret);
        }

        ltgbuf_merge(&node->send_buf, buf);

#if 1
        int ev;
        __corenet_event_cleanup(node, &ev);

        ret = __corenet_tcp_send(node);
        if (unlikely(ret)) {
                DWARN("send to %d fail\n", sockid->sd);
        }

        __corenet_event_set(node, ev);
#endif

        if (!list_empty(&node->send_buf.list)) {
                __corenet_set_out(node);
        }

        __corenet_tcp_unlock(node);

        return 0;
err_lock:
        __corenet_tcp_unlock(node);
err_ret:
        return ret;
}

static void __corenet_tcp_commit_task(void *ctx)
{
        struct list_head *pos, *n;
        corenet_fwd_t *corenet_fwd;
        corenet_tcp_t *__corenet__ = __corenet_get_byctx(ctx);
        struct list_head list;

        INIT_LIST_HEAD(&list);
        list_splice_init(&__corenet__->corenet.forward_list, &list);

        list_for_each_safe(pos, n, &list) {
                corenet_fwd = (void *)pos;
                list_del(pos);

                DBUG("forward to %s @ %u\n",
                     _inet_ntoa(corenet_fwd->sockid.addr), corenet_fwd->sockid.sd);

                __corenet_tcp_commit(&corenet_fwd->sockid, &corenet_fwd->buf);

                slab_stream_free(corenet_fwd);
        }
}

void corenet_tcp_commit(void *ctx)
{
        corenet_tcp_t *__corenet__ = __corenet_get_byctx(ctx);

        if (list_empty(&__corenet__->corenet.forward_list))
                return;

        sche_task_new("corenet_tcp_commit", __corenet_tcp_commit_task, ctx, -1);
        sche_run(core_tls_get(ctx, VARIABLE_SCHEDULE));
}

#else

inline static void __corenet_tcp_exec_send_nowait(void *_node)
{
        int ret;
        corenet_node_t *node = _node;
        sockid_t sockid = node->sockid;

        ret =  __corenet_tcp_send(node);
        if (unlikely(ret)) {
                DWARN("send to %d fail\n", sockid.sd);
        }

        if (!list_empty(&node->send_buf.list)) {
                __corenet_set_out(node);
        }

        return;
}

static int __corenet_tcp_commit(void *ctx, const sockid_t *sockid, ltgbuf_t *buf)
{
        int ret;
        corenet_node_t *node;
        corenet_tcp_t *__corenet__ = __corenet_get_byctx(ctx);

        LTG_ASSERT(sockid->type == SOCKID_CORENET);
        //LTG_ASSERT(sockid->addr);

        node = &__corenet__->array[sockid->sd];
        if (node->sockid.seq != sockid->seq || node->sockid.sd == -1) {
                ret = ECONNRESET;
                ltgbuf_free(buf);
                GOTO(err_ret, ret);
        }

        ltgbuf_merge(&node->send_buf, buf);

#if 1
        sche_task_new("corenet_tcp_send", __corenet_tcp_exec_send_nowait, node, -1);
        sche_run(core_tls_get(ctx, VARIABLE_SCHEDULE));
#else
#if 0
        ret =  __corenet_tcp_send(node);
        if (unlikely(ret)) {
                DWARN("send to %d fail\n", sockid->sd);
        }
#endif

        if (!list_empty(&node->send_buf.list)) {
                __corenet_set_out(node);
        }
#endif

        return 0;
err_ret:
        return ret;
}

void corenet_tcp_commit(void *ctx)
{
        struct list_head *pos, *n;
        corenet_fwd_t *corenet_fwd;
        corenet_tcp_t *__corenet__ = __corenet_get_byctx(ctx);

        list_for_each_safe(pos, n, &__corenet__->corenet.forward_list) {
                corenet_fwd = (void *)pos;
                list_del(pos);

                DBUG("forward to %s @ %u, buf %u\n",
                      _inet_ntoa(corenet_fwd->sockid.addr), corenet_fwd->sockid.sd,
                      corenet_fwd->buf.len);

                __corenet_tcp_commit(ctx, &corenet_fwd->sockid, &corenet_fwd->buf);

                slab_stream_free(corenet_fwd);
        }

        sche_run(core_tls_get(ctx, VARIABLE_SCHEDULE));
}

#endif

int corenet_tcp_connected(const sockid_t *sockid)
{
        int ret;
        corenet_node_t *node;
        corenet_tcp_t *__corenet__ = __corenet_get();

        if (sockid->sd == -1) {
                ret = ECONNRESET;
                GOTO(err_ret, ret);
        }

        node = &__corenet__->array[sockid->sd];
        if (node->sockid.seq != sockid->seq || node->sockid.sd == -1) {
                ret = ECONNRESET;
                GOTO(err_ret, ret);
        }

        return 1;
err_ret:
        return 0;
}

int corenet_tcp_init(int count, corenet_tcp_t **_corenet)
{
        int ret, len, i;
        corenet_tcp_t *corenet;
        corenet_node_t *node;

        len = sizeof(corenet_tcp_t) + sizeof(corenet_node_t) * count;

        DINFO("count %d size %d\n", count, len);

        ret = ltg_malloc((void **)&corenet, len);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        DBUG("corenet use %u\n", len);

        memset(corenet, 0x0, len);
        corenet->corenet.count = count;

        for (i = 0; i < count; i++) {
                node = &corenet->array[i];

#if ENABLE_TCP_THREAD
                __corenet_tcp_rwlock_init(node);
#endif

                ltgbuf_init(&node->recv_buf, 0);
                ltgbuf_init(&node->send_buf, 0);
                node->sockid.sd = -1;
        }

        corenet->corenet.epoll_fd = epoll_create(corenet->corenet.count);
        if (corenet->corenet.epoll_fd == -1) {
                ret = errno;
                GOTO(err_free, ret);
        }

        INIT_LIST_HEAD(&corenet->corenet.forward_list);
        INIT_LIST_HEAD(&corenet->corenet.check_list);

        ret = ltg_spin_init(&corenet->corenet.lock);
        if (unlikely(ret))
                GOTO(err_free, ret);


        core_tls_set(VARIABLE_CORENET_TCP, corenet);
        if (_corenet)
                *_corenet = corenet;

#if ENABLE_TCP_THREAD
        ret = __corenet_tcp_thread_init();
        if (unlikely(ret))
                GOTO(err_free, ret);
#endif

        DINFO("corenet init done corenet:%p\n", corenet);

	return 0;
err_free:
        ltg_free((void **)&corenet);
err_ret:
        return ret;
}
