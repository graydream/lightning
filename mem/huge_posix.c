#include "ltg_utils.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>

struct mem_alloc mem_posix;
int posix_alloc(void *meta_addr, void **_addr, uint32_t *size)
{
        int ret;
        void *addr;
        uint32_t req_size = _min(*size, mem_posix.max_alloc_size);

        (void)meta_addr;

        ret = ltg_malign((void **)&addr, PAGE_SIZE, req_size);
        if (ret)
                UNIMPLEMENTED(__DUMP__);

        *size = req_size;
        *_addr = addr;

        return 0;
}

int posix_free(void *meta_addr, void *addr, uint32_t size)
{
        (void)size;
        (void)meta_addr;
        free(addr);

        return 0;
}

int posix_init(void *addr, uint32_t size)
{
        (void)addr;
        (void)size;

        mem_posix.max_alloc_size = HUGEPAGE_SIZE * 4;

        return 0;
}

const struct mem_alloc *posix_memalloc_reg()
{
        return &mem_posix;
}

struct mem_alloc mem_posix = {
        .type = 0,      //todo, buddy type.
        .init = posix_init,
        .alloc = posix_alloc,
        .free = posix_free
};
