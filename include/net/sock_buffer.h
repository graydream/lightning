#ifndef __SOCK_BUFFER_H__
#define __SOCK_BUFFER_H__

#include <pthread.h>
#include <stdint.h>
#include <sys/uio.h>

#include "ltg_utils.h"
#include "ltg_net.h"

#define SOCK_IOV_MAX (128)

struct sockstate
{
	unsigned long long int bytes_send;
	unsigned long long int bytes_recv;
};

typedef struct{
        int fd; /*fd ref shm_t*/
        void *addr; /*begin of buffer*/
        uint32_t recv_off;
        uint32_t split_off;
} rbuf_seg_t;

typedef struct{
        ltg_spinlock_t lock;
        struct iovec iov[SOCK_IOV_MAX]; //iov for send
        ltgbuf_t buf;
} sock_rltgbuf_t;

typedef struct{
        ltg_spinlock_t lock;
        ltgbuf_t buf;
        struct iovec iov[SOCK_IOV_MAX]; //iov for send
        int closed;
} sock_wbuf_t;

#define BUFFER_KEEP 0x00000001

//extern void get_sockstate(struct sockstate *);
extern int sock_rbuffer_create(sock_rltgbuf_t *buf);
extern void sock_rbuffer_free(sock_rltgbuf_t *buf);
int sock_rbuffer_recv(sock_rltgbuf_t *sbuf, int sd);
//extern int sock_rbuffer_recv(sock_rltgbuf_t *buf, int sd, int *retry);
extern int sock_rbuffer_destroy(sock_rltgbuf_t *buf);
extern int sock_wbuffer_create(sock_wbuf_t *buf);
extern int sock_wbuffer_send(sock_wbuf_t *buf, int);
extern int sock_wbuffer_queue(sock_wbuf_t *wbuf,
                              const ltgbuf_t *buf);
int sock_wbuffer_destroy(sock_wbuf_t *buf);
int sock_wbuffer_isempty(sock_wbuf_t *sbuf);

#endif
