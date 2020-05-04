#ifndef __PLOCK_H__
#define __PLOCK_H__

#include <pthread.h>
#include <stdint.h>

#include "ltg_utils.h"

#define PLOCK_DEBUG 0
#define PLOCK_CHECK 0

typedef struct {
        struct list_head queue;
        int16_t writer;
        uint16_t readers;
#if PLOCK_CHECK
        int thread;
#endif
#if PLOCK_DEBUG
        int32_t count;
        uint32_t last_unlock;
        char name[MAX_LOCK_NAME];
#endif
} plock_t;

#if PLOCK_DEBUG
#define PLOCK_DUMP(lock) do {                                         \
                DINFO("lock %p (%s) writer %d reader %d count %u last %u\n", \
               (lock), \
               (lock)->name, \
               (lock)->writer, \
               (lock)->readers, \
               (lock)->count, \
               (lock)->last_unlock \
               ); \
} while(0)
#else
#define PLOCK_DUMP(lock) do { \
        DINFO("lock %p writer %d reader %d\n",  \
               (lock), \
               (lock)->writer, \
               (lock)->readers \
               ); \
} while(0)
#endif

/* plock.c */
extern int plock_init(plock_t *rwlock, const char *name);
extern int plock_destroy(plock_t *rwlock);
extern int plock_rdlock(plock_t *rwlock);
extern int plock_tryrdlock(plock_t *rwlock);
extern int plock_wrlock(plock_t *rwlock);
extern int plock_trywrlock(plock_t *rwlock);
extern void plock_unlock(plock_t *rwlock);
extern int plock_timedwrlock(plock_t *rwlock, int sec);
extern int plock_wrlock_prio(plock_t *rwlock, int prio);

#endif
