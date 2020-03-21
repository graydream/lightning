#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_utils.h"
#include "ltg_net.h"

typedef enum {
        STATUS_HEAD,
        STATUS_MSG,
        STATUS_IO_0,
        STATUS_IO_1,
        STATUS_FINISH,
} SBUF_STATUS_T;

const char *netable_rname(const nid_t *nid);
//extern mpool_t page_pool;

#ifdef LTG_DEBUG
//#define SOCK_BUFFER_DEBUG
#endif

volatile struct sockstate sock_state = {0, 0};

int sock_rbuffer_create(sock_rltgbuf_t *rbuf)
{
        int ret;

        ret = ltg_spin_init(&rbuf->lock);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        ret = ltgbuf_init(&rbuf->buf, 0);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int sock_rbuffer_destroy(sock_rltgbuf_t *rbuf)
{
        if (rbuf->buf.len) {
                DWARN("left %u\n", rbuf->buf.len);
        }

        ltgbuf_free(&rbuf->buf);
        return 0;
}

STATIC int __sock_rbuffer_recv(sock_rltgbuf_t *rbuf, int sd, int toread)
{
        int ret, iov_count;
        struct msghdr msg;
        ltgbuf_t buf;

        ret = ltgbuf_init(&buf, toread);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        LTG_ASSERT((uint32_t)buf.len <= (uint32_t)SOCK_IOV_MAX * BUFFER_SEG_SIZE);

        iov_count = SOCK_IOV_MAX;
        ret = ltgbuf_trans(rbuf->iov, &iov_count,  &buf);
        LTG_ASSERT(ret == (int)buf.len);        
        memset(&msg, 0x0, sizeof(msg));
        msg.msg_iov = rbuf->iov; 
        msg.msg_iovlen = iov_count;

        ret = _recvmsg(sd, &msg, MSG_DONTWAIT);
        if (ret < 0) {
                ret = -ret;
                GOTO(err_free, ret);
        }

        LTG_ASSERT(ret == (int)buf.len);
        DBUG("got data %u\n", ret);

        ltgbuf_merge(&rbuf->buf, &buf);

        return 0;
err_free:
        ltgbuf_free(&buf);
err_ret:
        return ret;
}

int sock_rbuffer_recv(sock_rltgbuf_t *rbuf, int sd)
{
        int ret;
        uint32_t toread, left, cp;

        DBUG("left %u, socket %u\n", rbuf->buf.len, sd);

        ret = ioctl(sd, FIONREAD, &toread);
        if (ret < 0) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        if (toread == 0) {
                ret = ECONNRESET;
                GOTO(err_ret, ret);
        }

        left = toread;
        while (left) {
                cp = left > (uint32_t)BUFFER_SEG_SIZE * SOCK_IOV_MAX ? (uint32_t)BUFFER_SEG_SIZE * SOCK_IOV_MAX : left;

                if (toread > (uint32_t)BUFFER_SEG_SIZE * SOCK_IOV_MAX) {
                        DINFO("long msg, total %u, left %u, read %u\n", toread, left, cp);
                }

                ret = __sock_rbuffer_recv(rbuf, sd, cp);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                left -= cp;
        }

        DBUG("recv %u\n", rbuf->buf.len);

        return 0;
err_ret:
        return ret;
}

int sock_wbuffer_create(sock_wbuf_t *wbuf)
{
        int ret;
        //memset(buf, 0x0, sizeof(sock_wbuf_t));

        wbuf->closed = 0;

        ret = ltg_spin_init(&wbuf->lock);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        ltgbuf_init(&wbuf->buf, 0);

        return 0;
err_ret:
        return ret;
}

STATIC int __sock_wbuffer_send__(struct iovec *iov, ltgbuf_t *buf, int sd)
{
        int ret, iov_count;
        struct msghdr msg;

        iov_count = SOCK_IOV_MAX;
        ret = ltgbuf_trans(iov, &iov_count,  buf);
        LTG_ASSERT(iov_count <= SOCK_IOV_MAX);
        memset(&msg, 0x0, sizeof(msg));
        msg.msg_iov = iov;
        msg.msg_iovlen = iov_count;

        ret = sendmsg(sd, &msg, MSG_DONTWAIT);
        if (ret < 0) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        DBUG("send %u\n", ret);

        return ret;
err_ret:
        return -ret;
}

STATIC int __sock_wbuffer_send(struct iovec *iov, ltgbuf_t *buf, int sd)
{
        int ret, sent;

        sent = 0;
        while (buf->len) {
                ret = __sock_wbuffer_send__(iov, buf, sd);
                if (ret < 0) {
                        ret = -ret;
                        if (ret == EAGAIN || ret == EWOULDBLOCK) {
                                break;
                        } else
                                GOTO(err_ret, ret);
                }

                sent += ret;

                if ((int)buf->len == ret) {
                        ltgbuf_free(buf);
                } else {
                        DBUG("pop %u from %u\n", ret, buf->len);
                        ltgbuf_pop(buf, NULL, ret);
                }
        }

        return sent;
err_ret:
        return -ret;
}

STATIC int sock_wbuffer_revert(sock_wbuf_t *wbuf, ltgbuf_t *buf)
{
        int ret;
        ltgbuf_t tmp;

        DWARN("left %u\n", buf->len);

        ret = ltg_spin_lock(&wbuf->lock);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        if (wbuf->buf.len) {
                DWARN("merge %u\n", wbuf->buf.len);

                ltgbuf_init(&tmp, 0);
                ltgbuf_merge(&tmp, &wbuf->buf);
                ltgbuf_merge(&wbuf->buf, buf);
                ltgbuf_merge(&wbuf->buf, &tmp);
        } else {
                ltgbuf_merge(&wbuf->buf, buf);
        }

        ltg_spin_unlock(&wbuf->lock);

        return 0;
err_ret:
        return ret;
}

//read form buf, write to sd
int sock_wbuffer_send(sock_wbuf_t *wbuf, int sd)
{
        int ret, count = 0;
        ltgbuf_t buf;

        DBUG("send by event\n");
        ltgbuf_init(&buf, 0);

        while (1) {
                ret = ltg_spin_lock(&wbuf->lock);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }

                ltgbuf_merge(&buf, &wbuf->buf);

                ltg_spin_unlock(&wbuf->lock);

                if (!buf.len) {
                        break;
                }

                ret = __sock_wbuffer_send(wbuf->iov, &buf, sd);
                if (ret < 0) {
                        ret = -ret;
                        //ltgbuf_free(&buf);
                        if (sock_wbuffer_revert(wbuf, &buf)) {
                                UNIMPLEMENTED(__DUMP__);
                        }

                        GOTO(err_ret, ret);
                }

                count += ret;

                if (buf.len) {
                        ret = sock_wbuffer_revert(wbuf, &buf);
                        if (unlikely(ret)) {
                                UNIMPLEMENTED(__DUMP__);
                        }

                        break;
                }
        }

        DBUG("send %u\n", count);

        return 0;
err_ret:
        return ret;
}

STATIC void __sock_wbuffer_queue(sock_wbuf_t *wbuf, const ltgbuf_t *buf)
{
        DBUG("queue msg %u\n", buf->len);

        ltgbuf_t tmp;
        ltgbuf_init(&tmp, 0);
        ltgbuf_clone_glob(&tmp, buf);
        ltgbuf_merge(&wbuf->buf, &tmp);
}

int sock_wbuffer_queue(sock_wbuf_t *wbuf, const ltgbuf_t *buf)
{
        int ret;

        LTG_ASSERT(buf->len);

        ret = ltg_spin_lock(&wbuf->lock);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        if (wbuf->closed) {
                ret = ECONNRESET;
                GOTO(err_lock, ret);
        }

        __sock_wbuffer_queue(wbuf, buf);

        ltg_spin_unlock(&wbuf->lock);

        return 0;
err_lock:
        ltg_spin_unlock(&wbuf->lock);
err_ret:
        return ret;
}

int sock_wbuffer_isempty(sock_wbuf_t *wbuf)
{
        int ret, empty;

        ret = ltg_spin_lock(&wbuf->lock);
        if (unlikely(ret)) {
                UNIMPLEMENTED(__DUMP__);
        }

        empty = !wbuf->buf.len;

        ltg_spin_unlock(&wbuf->lock);

        return empty;
}

int sock_wbuffer_destroy(sock_wbuf_t *wbuf)
{
        int ret;

        ret = ltg_spin_lock(&wbuf->lock);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        wbuf->closed = 1;

        if (wbuf->buf.len) {
                DWARN("left data %u\n", wbuf->buf.len);
        }

        ltgbuf_free(&wbuf->buf);

        ltg_spin_unlock(&wbuf->lock);

        return 0;
err_ret:
        return ret;
}
