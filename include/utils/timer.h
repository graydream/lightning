#ifndef __TIMER_H__
#define __TIMER_H__

#include "macros.h"

typedef int (*resume_func)(void *ctx, int retval);

typedef int (*timer_exec_t)(void *);

void timer_expire(void *ctx);
int timer_init(int private);
void timer_destroy();
int timer_insert(const char *name, void *ctx, func_t func, suseconds_t usec);

#endif
