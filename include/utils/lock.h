#ifndef __YLOCK_H__
#define __YLOCK_H__

#include <pthread.h>
#include <stdint.h>

#include "ltg_list.h"
#include "ltg_id.h"

#define MAX_LOCK_NAME 128

extern int __ltg_spin_lock(pthread_spinlock_t *lock, const char *name);

#if 1

#define ltg_spinlock_t pthread_spinlock_t

#define ltg_spin_init(__spin__) \
        pthread_spin_init(__spin__, PTHREAD_PROCESS_PRIVATE)

#define ltg_spin_destroy pthread_spin_destroy
#if 0
#define ltg_spin_lock(lock) __ltg_spin_lock(lock, __FUNCTION__)
#else
#define ltg_spin_lock pthread_spin_lock
#endif
#define ltg_spin_trylock pthread_spin_trylock
#define ltg_spin_unlock pthread_spin_unlock

#endif



#define SCHEDULE

#ifdef SCHEDULE

#if 1
#define LOCK_DEBUG 0
#endif

typedef struct {
        pthread_rwlock_t lock;
        struct list_head queue;
        ltg_spinlock_t spin;
        uint32_t priority;
#if LOCK_DEBUG
        int32_t count;
        uint32_t last_unlock;
        task_t writer;
        char name[MAX_LOCK_NAME];
#endif
} ltg_rwlock_t;

#endif

#endif
