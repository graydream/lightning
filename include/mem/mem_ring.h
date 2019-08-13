#ifndef __MEM_RING_H_
#define __MEM_RING_H_

#include "hugepage.h"

typedef struct {
        void *pool;
        void *head;
        void *ptr;
        uint64_t phyaddr;
} mem_handler_t;

int mem_ring_init();
int mem_ring_private_init(int corehash);
int mem_ring_new(uint32_t size, mem_handler_t *mem_handler);
int mem_ring_ref(mem_handler_t *mem_handler);
void mem_ring_deref(mem_handler_t *mem_handler);
void mem_ring_check(const mem_handler_t *mem_handler);

#endif
