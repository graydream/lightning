#ifndef __MEM_HUGEPAGE_H_
#define __MEM_HUGEPAGE_H_

//#include "ltgbuf.h"
struct mem_alloc {
	uint8_t type;
	uint32_t  max_alloc_size;
	int (*init)(void *addr, uint32_t);
	int (*alloc)(void *handle, void **addr, uint32_t *);
	int (*free)(void * handle, void *addr, uint32_t);
};

#define MAX_CPU_CORE 32
#if 0
#define PRIVATE_HP_COUNT 512
#endif
#define PUBLIC_HP_COUNT  (512)
#define HUGEPAGE_SIZE (2UL * 1024 * 1024)

int hugepage_init(int daemon, uint64_t coremask, int nr_huge);
void *hugepage_private_init(int hash, int sockid);

extern int hugepage_getfree(void **addr, uint32_t *size);

void get_global_private_mem(void **private_mem, uint64_t *private_mem_size);
int suzaku_mem_alloc_register(struct mem_alloc *alloc_ops);
void buddy_memalloc_reg();
void posix_memalloc_reg();

#define SUZAKU_MEM_ALLOC_REGISTER(name, mem_alloc_ops) \
	static void  __attribute__((constructor)) suzaku_mem_alloc_register_##name(void) \
{ \
	suzaku_mem_alloc_register(mem_alloc_ops); \
}\

#endif
