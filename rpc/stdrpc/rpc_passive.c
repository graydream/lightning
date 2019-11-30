#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>

#define DBG_SUBSYS S_LTG_RPC

#include "ltg_net.h"
#include "ltg_utils.h"
#include "ltg_rpc.h"

static int __pasv_sd__ = -1;
static int __pasv_port__ = -1;

#define NETINFO_TIMEOUT (10 * 60)

int rpc_getinfo(char *infobuf, uint32_t *infobuflen)
{
        int ret;
        uint32_t port = __pasv_port__;
        ltg_net_info_t *info;
        
        if (ltgconf_global.daemon && port == (uint32_t)-1) {
                ret = EAGAIN;
                GOTO(err_ret, ret);
        }

        while (ltgconf_global.daemon && ltg_global.local_nid.id == 0) {
                DWARN("wait nid inited\n");
                sleep(1);
        }

        ret = net_getinfo(infobuf, infobuflen, __pasv_port__, &ltg_netconf_manage);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        info = (ltg_net_info_t *)infobuf;
        DBUG("port %d, %u\n", ntohs(info->info[0].port), __pasv_port__);

        return 0;
err_ret:
        return ret;
}

typedef struct {
        net_handle_t newnh;
} arg_t;

static void *__rpc_accept__(void *_arg)
{
        int ret;
        char buf[MAX_BUF_LEN];
        ltg_net_info_t *info;
        net_handle_t newnh;
        net_proto_t proto;
        arg_t *arg;

        ANALYSIS_BEGIN(0);
        arg = _arg;
        newnh = arg->newnh;

        memset(&proto, 0x0, sizeof(net_proto_t));

        proto.head_len = sizeof(ltg_net_head_t);
        proto.pack_len = rpc_pack_len;
        proto.pack_handler = rpc_pack_handler;

        proto.reader = net_events_handle_read;
        proto.writer = net_events_handle_write;

        info = (void *)buf;
        ret = net_accept(&newnh, info, &proto);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        LTG_ASSERT(strcmp(info->name, "none"));

        if (net_isnull(&info->id) || net_islocal(&info->id)) {
                ret = sdevent_add(&newnh, NULL, LTG_EPOLL_EVENTS, NULL, NULL);
                if (unlikely(ret)) {
                        DINFO("accept from %s, sd %u ret:%d \n",
                              _inet_ntoa(newnh.u.sd.addr), newnh.u.sd.sd, ret);
                        GOTO(err_sd, ret);
                }
        } else {
                ret = netable_accept(info, &newnh);
                if (unlikely(ret)) {
                        DINFO("accept from %s(%s), sd %u ret:%d\n",
                               _inet_ntoa(newnh.u.sd.addr), info->name, newnh.u.sd.sd, ret);
                        GOTO(err_sd, ret);
                }
        }

        ANALYSIS_END(0, IO_WARN, NULL);

        ltg_free((void **)&_arg);

        pthread_exit(NULL);
err_sd:
        sdevent_close_force(&newnh);
err_ret:
        ltg_free((void **)&_arg);
        pthread_exit(NULL);
}

static int __rpc_accept()
{
        int ret;
        net_handle_t newnh;
        arg_t *arg;

        ret = sock_accept(&newnh, __pasv_sd__, 1, 0);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = ltg_malloc((void **)&arg, sizeof(*arg));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        arg->newnh = newnh;

        ret = ltg_thread_create(__rpc_accept__, arg, "__rpc_accept");
        if (unlikely(ret))
                GOTO(err_free, ret);

        return 0;
err_free:
        ltg_free((void **)&arg);
err_ret:
        return ret;
}

static void *__rpc_accept_worker(void *_arg)
{
        int ret;
        sem_t *sem;

        sem = _arg;

        ret = sem_post(sem);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        main_loop_hold();

        while (1) {
                ret = sock_poll_sd(__pasv_sd__, 1000 * 1000, POLLIN);
                if (unlikely(ret)) {
                        if (ret == ETIMEDOUT || ret == ETIME)
                                continue;
                        else
                                GOTO(err_ret, ret);
                }

                ret = __rpc_accept();
                if (unlikely(ret)) {
                        ret = _errno(ret);
                        if (ret == EAGAIN)
                                continue;
                        else
                                UNIMPLEMENTED(__DUMP__);
                }
        }

        return NULL;
err_ret:
        UNIMPLEMENTED(__DUMP__);
        return NULL;
}

int rpc_passive(uint32_t port)
{
        int ret, sd;
        net_handle_t nh;
        char _port[MAX_LINE_LEN];

        memset(&nh, 0x0, sizeof(nh));

        LTG_ASSERT(__pasv_sd__ == -1);

        if (port != (uint32_t)-1) {
                snprintf(_port, MAX_LINE_LEN, "%u", port);

                LTG_ASSERT(port > LNET_SERVICE_RANGE && port < 65535);
                ret = tcp_sock_hostlisten(&sd, NULL, _port,
                                          256, 0, 1);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }
        } else {
                port = LNET_PORT_RANDOM;
                while (srv_running) {
                        port = (uint16_t)(LNET_SERVICE_BASE
                                          + (random() % LNET_SERVICE_RANGE));

                        LTG_ASSERT(port > LNET_SERVICE_RANGE && port < 65535);
                        snprintf(_port, MAX_LINE_LEN, "%u", port);

                        ret = tcp_sock_hostlisten(&sd, NULL, _port,
                                                  256, 0, 1);
                        if (unlikely(ret)) {
                                if (ret == EADDRINUSE) {
                                        DBUG("port (%u + %u) %s\n", LNET_SERVICE_BASE,
                                             port - LNET_SERVICE_BASE, strerror(ret));
                                        continue;
                                } else
                                        GOTO(err_ret, ret);
                        } else {
                                break;
                        }
                }
        }

        __pasv_sd__ = sd;
        __pasv_port__ = port;

        DINFO("listen %u, nid %u\n", port, net_getnid()->id);

        return 0;
err_ret:
        return ret;
}

int rpc_start()
{
        int ret;
        sem_t sem;

        while (__pasv_sd__ == -1) {
                DWARN("wait rpc passive inited\n");
                sleep(1);
        }

        ret = sem_init(&sem, 0, 0);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = ltg_thread_create(__rpc_accept_worker, &sem, "__rpc_accept_worker");
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = sem_wait(&sem);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}
