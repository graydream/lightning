#ifndef __RDMA_EVENT_H__
#define __RDMA_EVENT_H__

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include "ltg_utils.h"

enum {
        ISER_EV_FD,
        RDMA_SERVER_EV_FD,
        RDMA_CLIENT_EV_FD,
        EV_FD_END,
};

typedef void (*event_handle_t)(int fd, int type, int events, void *data, void *private_mem);
struct event_data;
typedef void (*sched_event_handler_t)(struct event_data *tev);
typedef void (rdma_event_handler_t)(struct rdma_cm_event *, void *);
typedef int (*sched_remains)();

struct event_data {
        union {
                event_handle_t handler;
                sched_event_handler_t sched_handler;
        };
        union {
                int fd;
                int scheduled;
        };
        int type;
        void *data;
        void *private_mem;
        void *core;
        struct list_head e_list;
};

int rdma_event_init();

void rdma_event_register(int type,
                         rdma_event_handler_t connect,
                         rdma_event_handler_t established,
                         rdma_event_handler_t disconnected,
                         rdma_event_handler_t timewait_exit,
                         sched_remains remains);

void *rdma_event_loop(void *arg);
void rdma_handle_event(int fd, int type,
                int events __attribute__ ((unused)),
                void *data __attribute__ ((unused)),
                void *core);

int rdma_event_add(int fd, int type, int event, event_handle_t handler,
                   void *data, void *private_mem);
void rdma_event_del(int fd);
int rdma_event_modify(int fd, int events);

#endif
