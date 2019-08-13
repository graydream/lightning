#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_net.h"

/**
 * EPOLLIN
 */
int net_events_handle_read(void *_sock, void *ctx)
{
        int ret, msg_len, io_len;
        ltg_sock_conn_t *sock = _sock;
        ltgbuf_t *buf, _buf;
        char tmp[MAX_BUF_LEN];
        sock_rltgbuf_t *rbuf;

        (void) ctx;

        rbuf = &sock->rbuf;
        ret = ltg_spin_trylock(&rbuf->lock);
        if (unlikely(ret)) {
                goto out;
        }

        ret = sock_rbuffer_recv(rbuf, sock->nh.u.sd.sd);
        if (unlikely(ret)) {
                goto err_lock;
        }

        DBUG("recv %u from %s\n", sock->rbuf.buf.len, _inet_ntoa(sock->nh.u.sd.addr));

        buf = &rbuf->buf;
        while (buf->len >= sock->proto.head_len) {
                ltgbuf_get(buf, tmp, sock->proto.head_len);
                sock->proto.pack_len(tmp, sock->proto.head_len, &msg_len, &io_len);

#if 0
                ltg_net_head_t *head = (void *)tmp;
                DINFO("new msg from %s, id (%u, %x), need %u got %u, socket %u\n",
                      _inet_ntoa(sock->nh.u.sd.addr), head->msgid.idx,
                      head->msgid.figerprint, msg_len + io_len, buf->len, sock->nh.u.sd.sd);
#endif


                if (msg_len + io_len > (int)buf->len) {
                        DBUG("wait %u %u\n", msg_len + io_len, buf->len);
                        break;
                }

                ltgbuf_init(&_buf, 0);
                ltgbuf_pop1(buf, &_buf, msg_len + io_len, 1);

                DBUG("new msg %u from %s\n", _buf.len, _inet_ntoa(sock->nh.u.sd.addr));

                ret = sock->proto.pack_handler(sock->nid, &sock->nh.u.sd, &_buf);
                if (unlikely(ret)) {
                        GOTO(err_lock, ret);
                }
        }

        ltg_spin_unlock(&rbuf->lock);

out:
        return 0;
err_lock:
        ltg_spin_unlock(&rbuf->lock);
//err_ret:
        return ret;
}

/**
 * EPOLLOUT
 */
int net_events_handle_write(event_t *ev, void *context)
{
        int ret;
        ltg_sock_conn_t *sock;

        (void) ev;

        sock = context;

        DBUG("send %u to %s\n", sock->wbuf.buf.len, _inet_ntoa(sock->nh.u.sd.addr));

        ret = sock_wbuffer_send(&sock->wbuf, sock->nh.u.sd.sd);
        if (ret < 0) {
                ret = -ret;
                GOTO(err_ret, ret);
        }

        return ret;
err_ret:
        return -ret;
}
