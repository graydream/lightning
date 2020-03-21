#ifndef __SDEVENT_H__
#define __SDEVENT_H__

//#include "config.h"

/*
sdevent is a special fast array. it is lock free.
only for save value which have a key of fd or other small unequal int.
be careful to use it
*/
#include <sys/epoll.h>
#include <semaphore.h>
#include <pthread.h>

#include "ltg_net.h"
#include "ltg_utils.h"
#include "net_proto.h"
#include "sock_lib.h"

typedef struct epoll_event event_t;
typedef int (*event_handler_t)(event_t *);
typedef int (*event_handler_func)(event_t *);


int sdevent_init(int max);
void sdevent_destroy(void);

int sdevent_open(net_handle_t *nh, const net_proto_t *proto);
void sdevent_close(const net_handle_t *nh);
int sdevent_connect(const sock_info_t *info, net_handle_t *nh,
                    net_proto_t *proto, int nonblock, int timeout);
int sdevent_add(const net_handle_t *socknh, const nid_t *nid, int event);
int sdevent_queue(const net_handle_t *nh, const ltgbuf_t *buf);
int sdevent_check(const sockid_t *id);
int sdevent_recv(int fd);
void sdevent_exit(int fd);

#endif
