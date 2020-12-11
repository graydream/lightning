#ifndef __ANALYSIS_H__
#define __ANALYSIS_H__

#include "htab.h"
#include "lock.h"

#define ANALYSIS_QUEUE_MAX (8192 * 100)

typedef struct {
        int count;
        struct {
                char name[MAX_NAME_LEN];
                uint64_t time;
        } array[ANALYSIS_QUEUE_MAX];
} analysis_queue_t;

typedef struct {
        struct list_head hook;
        char name[MAX_NAME_LEN];
        int private;
        htab_t tab;
        analysis_queue_t *queue;
        analysis_queue_t *new_queue;
        ltg_spinlock_t queue_lock;
        ltg_spinlock_t tab_lock;
} analysis_t;

typedef struct {
        char name[MAX_NAME_LEN];
} analysis_entry_t;

extern analysis_t *default_analysis;

int analysis_dumpall(void);
int analysis_queue(analysis_t *ana, const char *name, const char *type, uint64_t _time);
int analysis_init();
int analysis_dump(const char *tab, const char *name,  char *buf);
int analysis_private_queue(const char *_name, const char *type, uint64_t _time);
void analysis_merge(void *ctx);

#endif
