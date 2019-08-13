#ifndef __MEM_HUGEPAGE_H_
#define __MEM_HUGEPAGE_H_

//#include "ltgbuf.h"

#define MAX_CPU_CORE 32
#define PRIVATE_HP_COUNT 512
#define PUBLIC_HP_COUNT  (64)
#define HUGEPAGE_SIZE (2UL * 1024 * 1024)

int hugepage_init(int daemon, uint64_t coremask, int use_huge);
void *hugepage_private_init(int hash, int sockid);
void get_global_private_mem(void **private_mem, uint64_t *private_mem_size);
extern int hugepage_getfree(void **addr, uint64_t *phyaddr);

#endif
