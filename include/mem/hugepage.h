#ifndef __MEM_HUGEPAGE_H_
#define __MEM_HUGEPAGE_H_

//#include "ltgbuf.h"
#include "utils/lock.h"
#define MAX_CPU_CORE 32

#define HUGEPAGE_SIZE (2UL * 1024 * 1024)
#define MAX_SIZE 2
#define MAX_NUMA 8
#define MIN_MEMSEG_SIZE (1024 * 1024 * 1024)
#define IS_POWER_OF_2(x) (!((x)&((x)-1)))
#define RDMA_MEMREG_SIZE MIN_MEMSEG_SIZE
#define MEMSLICE_SIZE (128 * 1024 * 1024)
#define MAX_ALLOC_SIZE (MAX_SIZE * HUGEPAGE_SIZE)

typedef struct {
        struct  list_head memseg_list;
        struct  list_head  slice_list;

        void             *malloc_addr;
        void             *start_addr;

        int              numa;
        size_t           size;
        ltg_spinlock_t    lock;
} memseg_t;

typedef struct {
        struct list_head list;
        void *slice;
        void *vaddr;
        void *head;
        uint32_t size;
        int ref;
        int type;
        int offset;
}hpage_t;

/*memslice size 128MB, thread cache object*/
typedef struct {
        struct list_head list;
        int  map;
        void *addr;
        memseg_t *memseg;
        uint32_t hp_size;
        int hp_count;
        hpage_t hpage[MEMSLICE_SIZE / HUGEPAGE_SIZE];
}memslice_t;

typedef struct {
        struct list_head list[MAX_SIZE];
        int numa;
        ltg_spinlock_t  hp_lock;
}hpage_head_t;

int memseg_init(int daemon, int nr_hugepage);
int private_hugepage_init(int numa, void **ptr);
int hugepage_getfree(void **addr, uint32_t *size, const char *caller);
hpage_t* hpage_get(void *hp_list, int offset);
int hugepage_init(void **ptr);
void hpage_free(void *hp_list, int offset, hpage_t *hpage);
int memseg_walk(int (*func)(void *addr, void *arg, size_t size), void *arg);
#endif
