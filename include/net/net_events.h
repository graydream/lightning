#ifndef __NET_EVENTS_H__
#define __NET_EVENTS_H__

#include "sdevent.h"

#define LTG_EPOLL_EVENTS (EPOLLIN | EPOLLERR | EPOLLRDHUP)

int net_events_handle_read(void *sock, void *ctx);
int net_events_handle_write(event_t *ev, void *ctx);

#endif
