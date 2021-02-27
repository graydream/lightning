#ifndef __QD__
#define __QD__

#include <sys/epoll.h>
#include <semaphore.h>
#include <linux/aio_abi.h> 
#include <pthread.h>

typedef int (*qd_func_t)(void *);

int qd_init();
void qd_register(void *ctx, char *name, qd_func_t func);


#endif
