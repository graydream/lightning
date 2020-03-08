#ifndef __HEART_BEAT_H__
#define __HEART_BEAT_H__

#include "net_lib.h"

int heartbeat_add(const sockid_t *sockid, const nid_t *parent, suseconds_t timeout, time_t ltime);

int heartbeat_add1(const sockid_t *sockid, const char *name, void *ctx,
                   int (*connected)(void *), int (*send)(void *), int (*close)(void *),
                   suseconds_t timeout, int retry);
int heartbeat_init();

#endif
