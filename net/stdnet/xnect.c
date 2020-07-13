#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_net.h"
#include "ltg_utils.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

extern net_proto_t net_proto;

ssize_t _send(int sockfd, const void *buf, size_t len, int flags)
{
        int ret;

        while (1) {
                ret = send(sockfd, buf, len, flags);
                if (ret == -1) {
                        ret = errno;

                        if (ret == EINTR) {
                                DERROR("interrupted");
                                continue;
                        } else if (ret == EAGAIN || ret == ECONNREFUSED
                                 || ret == EHOSTUNREACH)
                                goto err_ret;
                        else
                                GOTO(err_ret, ret);
                }

                break;
        }

        return ret;
err_ret:
        return -ret;
}

ssize_t _recv(int sockfd, void *buf, size_t len, int flags)
{
        int ret;

        while (1) {
                ret = recv(sockfd, buf, len, flags);
                if (ret == -1) {
                        ret = errno;

                        if (ret == EINTR) {
                                DERROR("interrupted");
                                continue;
                        } else
                                goto err_ret;
                }

                break;
        }

        if (ret == 0) {
                ret = ECONNRESET;
                goto err_ret;
        }

        return ret;
err_ret:
        return -ret;
}

static int __sock_connect(net_handle_t *nh, const sock_info_t *info, const nid_t *peerid,
                          const void *localinfo, uint32_t infolen, int timeout)
{
        int ret;
        char buf[MAX_BUF_LEN];

        LTG_ASSERT(timeout <= 1000 * 1000 && timeout >= 100 * 1000);

        ret = sock_info2sock(nh, info, 0, timeout);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        DBUG("conneted sd %u\n", nh->u.sd.sd);

        ret = _send(nh->u.sd.sd, (void *)localinfo, infolen,
                    MSG_NOSIGNAL | MSG_DONTWAIT);
        if (ret < 0) {
                ret = -ret;
                GOTO(err_fd, ret);
        } else if ((uint32_t)ret != infolen) {
                ret = EBADF;
                DWARN("bad sd %u\n", nh->u.sd.sd);
                GOTO(err_fd, ret);
        }

        ret = sock_poll_sd(nh->u.sd.sd, timeout, POLLIN);
        if (unlikely(ret))
                GOTO(err_fd, ret);

        ret = _recv(nh->u.sd.sd, (void *)buf, MAX_BUF_LEN, MSG_DONTWAIT);
        if (ret < 0) {
                ret = errno;
                GOTO(err_fd, ret);
        }

        if (ret == 0) {
                ret = ECONNRESET;
                GOTO(err_fd, ret);
        }

        ltg_net_info_t *peerinfo = (void *)buf;
        if (nid_cmp(&peerinfo->id, peerid)) {
                ret = EINVAL;
                DERROR("remote is %s\n", netable_rname(&peerinfo->id));
                GOTO(err_fd, ret);
        }
        
        ret = sock_setnonblock(nh->u.sd.sd);
        if (unlikely(ret)) {
                DERROR("%d - %s\n", ret, strerror(ret));
                GOTO(err_fd, ret);
        }

        ret = sdevent_open(nh, &net_proto);
        if (unlikely(ret))
                GOTO(err_fd, ret);
        
        return 0;
err_fd:
        close(nh->u.sd.sd);
err_ret:
        return ret;
}

int net_connect(net_handle_t *sock, const ltg_net_info_t *info, int timeout)
{
        int ret;
        uint32_t infolen;
        char buf[MAX_BUF_LEN];

        LTG_ASSERT(timeout <= 1000 * 1000 && timeout >= 100 * 1000);
        LTG_ASSERT(!sche_running());

        infolen = MAX_BUF_LEN;
        ret = rpc_getinfo(buf, &infolen);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        ret = __sock_connect(sock, &info->info[0], &info->id, buf, infolen,
                             timeout);
        if (unlikely(ret)) {
                ret = ENONET;
                GOTO(err_ret, ret);
        }

        LTG_ASSERT(sock->u.sd.type == SOCKID_NORMAL);
        
        return 0;
err_ret:
        return ret;
}

static int __peek(int sd, char *buf, uint32_t buflen)
{
        int ret;
        uint32_t toread;

        ret = ioctl(sd, FIONREAD, &toread);
        if (ret == -1) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        toread = toread < buflen ? toread : buflen;

        ret = recv(sd, buf, toread, MSG_PEEK);
        if (ret == -1) {
                ret = errno;
                DERROR("peek errno %d\n", ret);
                GOTO(err_ret, ret);
        }

        if (ret == 0) {
                ret = ECONNRESET;
                goto err_ret;
        }

        return ret;
err_ret:
        return -ret;
}

int net_accept(net_handle_t *nh, ltg_net_info_t *info, const net_proto_t *proto)
{
        int ret, newsd;
        uint32_t buflen = MAX_BUF_LEN;
        char buf[MAX_BUF_LEN];

        newsd = nh->u.sd.sd;

retry:
        ret = sock_poll_sd(newsd, (ltgconf_global.rpc_timeout / 2) * 1000 * 1000, POLLIN );
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = __peek(newsd, (void *)info, MAX_BUF_LEN);
        if (ret < 0) {
                ret = -ret;
                GOTO(err_ret, ret);
        }

        if (ret < (int)sizeof(info->len)) {
                DWARN("got ret %u\n", ret);
                goto retry;
        }

        ret = _recv(newsd, (void *)info, info->len, MSG_DONTWAIT);
        if (ret < 0) {
                ret = -ret;
                GOTO(err_ret, ret);
        } else if ((uint32_t)ret != info->len) {
                ret = EBADF;
                GOTO(err_ret, ret);
        }

        ret = rpc_getinfo(buf, &buflen);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = _send(newsd, buf, buflen, 0);
        if (ret < 0) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        if (ret != (int)buflen) {
                ret = ECONNRESET;
                GOTO(err_ret, ret);
        }

        LTG_ASSERT(info->len);

        ret = sdevent_open(nh, proto);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = sock_setnonblock(newsd);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        return 0;
err_ret:
        return ret;
}
