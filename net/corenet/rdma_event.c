#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <rdma/rdma_cma.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "ltg_utils.h"
#include "ltg_core.h"

#define EPOLL_FD_NUM_MAX 4096
#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))

static int rdma_epoll_fd;

static LIST_HEAD(events_list);
static int __nr_fds = 0;

struct rdma_handle_op_t {
        void (*rdma_connect_requtest)(struct rdma_cm_event *, void *);
        void (*rdma_established_requtest)(struct rdma_cm_event *, void *);
        void (*rdma_disconnected_requtest)(struct rdma_cm_event *, void *);
        void (*rdma_timewait_exit_requtest)(struct rdma_cm_event *, void *);
};

static struct rdma_handle_op_t rdma_request_op[EV_FD_END];

void rdma_request_do_nothing(struct rdma_cm_event *ev, void *core)
{
        (void) ev;
        (void) core;

        return;
}

static struct event_data *rdma_event_lookup(int fd)
{
        struct event_data *tev;

        list_for_each_entry(tev, &events_list, e_list) {
                if (tev->fd == fd)
                        return tev;
        }
        return NULL;
}

int rdma_event_add(int fd, int type, int event, event_handle_t handler,
                   void *data, void *core)
{
        int ret;
        struct epoll_event ev;
        struct event_data *tev;

        ret = ltg_malloc((void **)&tev, sizeof(*tev));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        tev->fd = fd;
        tev->type = type;
        tev->data = data;
        tev->core = core;
        tev->handler = handler;

        memset(&ev, 0, sizeof(ev));
        ev.events = event;
        ev.data.ptr = tev;
        ret = epoll_ctl(rdma_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
        if (unlikely(ret < 0)) {
                ret = errno;
                DWARN("add fd: %d to epool fd: %d\n", fd, rdma_epoll_fd);
                GOTO(err_free, ret);
        }

        list_add(&tev->e_list, &events_list);
        __nr_fds++;

        DINFO("add type %d fd %d nr %d\n", type, fd, __nr_fds);
        return 0;
err_free:
        ltg_free((void **)&tev);
err_ret:
        return ret;
}

void rdma_event_del(int fd)
{
        struct event_data *tev;
        int ret;

        tev = rdma_event_lookup(fd);
        if (unlikely(!tev)) {
                DERROR("Cannot find event %d\n", fd);
                return;
        }

        ret = epoll_ctl(rdma_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        if (unlikely(ret < 0))
                DERROR("fail to remove epoll event, %s\n", strerror(errno));

        list_del(&tev->e_list);
        __nr_fds--;

        ltg_free1(tev);

        DINFO("del fd %d nr %d\n", fd, __nr_fds);
}

int rdma_event_modify(int fd, int events)
{
        struct epoll_event ev;
        struct event_data *tev;

        tev = rdma_event_lookup(fd);
        if (unlikely(!tev)) {
                DERROR("Cannot find event %d\n", fd);
                return -EINVAL;
        }

        memset(&ev, 0, sizeof(ev));
        ev.events = events;
        ev.data.ptr = tev;

        return epoll_ctl(rdma_epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

sched_remains __sched_remains__ = NULL;

void *rdma_event_loop(void *arg)
{
        int nevent, i;
        int timeout = 5;
        struct epoll_event events[1024];
        struct event_data *tev;

        (void)arg;
        (void) timeout;
        
        int sched_remains;
        
retry:
        if (__sched_remains__) {
                sched_remains = __sched_remains__();
                timeout = sched_remains ? 0 : -1;
        }
        
        nevent = epoll_wait(rdma_epoll_fd, events, ARRAY_SIZE(events), timeout);
        if (unlikely(nevent < 0)) {
                if (errno != EINTR) {
                        DERROR("%m\n");
                        EXIT(1);
                }
        } else {
                for (i = 0; i < nevent; i++) {
                        tev = (struct event_data *) events[i].data.ptr;
                        tev->handler(tev->fd, tev->type, events[i].events, tev->data, tev->core);
                }
        }

        if (likely(rdma_running))
                goto retry;

        return NULL;
}

void rdma_handle_event(int fd, int type,
                       int events __attribute__ ((unused)),
                       void *data __attribute__ ((unused)),
                       void *core)
{
        int ret, ack = 1;
        struct rdma_cm_event *ev;
        enum rdma_cm_event_type ev_type;
        struct rdma_event_channel rdma_evt_channel;
        struct rdma_cm_id *cm_id;
        rdma_conn_t *handler;
        struct sockaddr_in *sockaddr;

        DINFO("corenet rdma:rdma handle event\n");

        ANALYSIS_BEGIN(0);

        rdma_evt_channel.fd = fd;
        ret = rdma_get_cm_event(&rdma_evt_channel, &ev);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ev_type = ev->event;
        cm_id = ev->id;

        CMID_DUMP_L(DINFO, cm_id);

        handler = cm_id->context;
        if (handler) {
                handler->nr_get++;
                RDMA_CONN_DUMP_L(DINFO, handler);
        }

        sockaddr = (struct sockaddr_in *)(&cm_id->route.addr.dst_addr);

        DBUG("iser fd[%d], type:%d, UD-related event:%d, %s, core:%d\n", fd, type,
             ev_type, rdma_event_str(ev_type), core ? ((core_t *)core)->hash : -1);

        switch (ev_type) {
        case RDMA_CM_EVENT_CONNECT_REQUEST:
                DINFO("connect request from %s:%d fd %d\n",
                      inet_ntoa(sockaddr->sin_addr),
                      ntohs(sockaddr->sin_port),
                      fd);
                rdma_request_op[type].rdma_connect_requtest(ev, core);
                break;

        case RDMA_CM_EVENT_ESTABLISHED:
                if (type == RDMA_SERVER_EV_FD) {
                        DINFO("%s:%d established on passive side, fd %d\n",
                              inet_ntoa(sockaddr->sin_addr),
                              (sockaddr->sin_port),
                              fd);
                } else if (type == RDMA_CLIENT_EV_FD) {
                        DINFO("%s:%d established on client side, fd %d\n",
                              inet_ntoa(sockaddr->sin_addr),
                              ntohs(sockaddr->sin_port),
                              fd);
                }
                rdma_request_op[type].rdma_established_requtest(ev, core);
                break;

        case RDMA_CM_EVENT_ADDR_CHANGE:
                break;
        case RDMA_CM_EVENT_CONNECT_ERROR:
        case RDMA_CM_EVENT_REJECTED:
        case RDMA_CM_EVENT_DISCONNECTED:
                DINFO("ev_type %d %s:%d disconnected request fd %d\n",
                      ev_type,
                      inet_ntoa(sockaddr->sin_addr),
                      ntohs(sockaddr->sin_port),
                      fd);
                rdma_request_op[type].rdma_disconnected_requtest(ev, core);
                break;

        case RDMA_CM_EVENT_TIMEWAIT_EXIT:
                DINFO("%s:%d timewait exit request fd %d\n",
                      inet_ntoa(sockaddr->sin_addr),
                      ntohs(sockaddr->sin_port),
                      fd);
                rdma_request_op[type].rdma_timewait_exit_requtest(ev, core);
                if (type == RDMA_CLIENT_EV_FD || type == RDMA_SERVER_EV_FD) {
                        ack = 0;
                }
                break;

        case RDMA_CM_EVENT_MULTICAST_JOIN:
        case RDMA_CM_EVENT_MULTICAST_ERROR:
                DERROR("UD-related event:%d, %s - ignored\n", ev_type,
                        rdma_event_str(ev_type));
                break;

        case RDMA_CM_EVENT_DEVICE_REMOVAL:
                DERROR("Unsupported event:%d, %s - ignored\n", ev_type,
                        rdma_event_str(ev_type));
                break;

        case RDMA_CM_EVENT_ADDR_RESOLVED:
        case RDMA_CM_EVENT_ADDR_ERROR:
        case RDMA_CM_EVENT_ROUTE_RESOLVED:
        case RDMA_CM_EVENT_ROUTE_ERROR:
        case RDMA_CM_EVENT_CONNECT_RESPONSE:
        case RDMA_CM_EVENT_UNREACHABLE:
                DERROR("Active side event:%d, %s - ignored\n", ev_type,
                        rdma_event_str(ev_type));
                break;

        default:
                DERROR("Illegal event:%d - ignored\n", ev_type);
                break;
        }

        if (ack) {
                ret = rdma_ack_cm_event(ev);
                if (unlikely(ret))
                        DERROR("ack cm event failed, %s\n", rdma_event_str(ev_type));

                if (handler) {
                        handler->nr_ack++;
                        RDMA_CONN_DUMP_L(DINFO, handler);
                }
        }

        ANALYSIS_END(0, 1000 * 1000 * 5, NULL);
        DINFO("ev_type %d successfully.\n", ev_type);
        return;
err_ret:
        ANALYSIS_END(0, 1000 * 1000 * 5, NULL);
        DERROR("failed\n");
        return;
}

int rdma_event_init()
{
        int ret;

        rdma_epoll_fd = epoll_create(EPOLL_FD_NUM_MAX);
        if (rdma_epoll_fd < 0) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        DINFO("rdma epoll fd: %d\n", rdma_epoll_fd);


        rdma_request_op[RDMA_SERVER_EV_FD].rdma_connect_requtest = corenet_rdma_connect_request;
        rdma_request_op[RDMA_SERVER_EV_FD].rdma_established_requtest = corenet_rdma_established;
        rdma_request_op[RDMA_SERVER_EV_FD].rdma_disconnected_requtest = corenet_rdma_disconnected;
        rdma_request_op[RDMA_SERVER_EV_FD].rdma_timewait_exit_requtest = corenet_rdma_timewait_exit;

        rdma_request_op[RDMA_CLIENT_EV_FD].rdma_connect_requtest = rdma_request_do_nothing;
        rdma_request_op[RDMA_CLIENT_EV_FD].rdma_established_requtest = rdma_request_do_nothing;
        rdma_request_op[RDMA_CLIENT_EV_FD].rdma_disconnected_requtest = corenet_rdma_disconnected;
        rdma_request_op[RDMA_CLIENT_EV_FD].rdma_timewait_exit_requtest = corenet_rdma_timewait_exit;

        ret = ltg_thread_create(rdma_event_loop, NULL, "rdma");
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

void rdma_event_register(int type,
                         rdma_event_handler_t connect,
                         rdma_event_handler_t established,
                         rdma_event_handler_t disconnected,
                         rdma_event_handler_t timewait_exit,
                         sched_remains remains)
{
        rdma_request_op[type].rdma_connect_requtest = connect;
        rdma_request_op[type].rdma_established_requtest = established;
        rdma_request_op[type].rdma_disconnected_requtest = disconnected;
        rdma_request_op[type].rdma_timewait_exit_requtest = timewait_exit;
        __sched_remains__ = remains;
}
