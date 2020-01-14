#define JEMALLOC_NO_DEMANGLE

#include <sys/types.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <numaif.h>

#define DBG_SUBSYS S_LTG_MEM

#include "ltg_utils.h"
#include "core/core.h"

#define powerof2(x)     ((((x) - 1) & (x)) == 0)

inline static void *__malloc__(size_t size)
{
#if ENABLE_JEM
        return je_malloc(size);
#else
        return malloc(size);
#endif
}

static void *__calloc__(size_t n, size_t elem_size)
{
#if ENABLE_JEM
        return je_calloc(n, elem_size);
#else
        return calloc(n, elem_size);
#endif
}

static void *__memalign__(size_t alignment, size_t bytes)
{
        int ret;
        void *ptr=NULL;

#if ENABLE_JEM
        ret = je_posix_memalign(&ptr, alignment, bytes);
#else
        ret = posix_memalign(&ptr, alignment, bytes);
#endif
        if (ret)
                return NULL;

        return ptr;
}

static void __free__(void *mem)
{
#if ENABLE_JEM
        return je_free(mem);
#else
        return free(mem);
#endif
}

void __ltg_malloc_bind(void *ptr, size_t size)
{

        core_t *core = core_self();
        if (core && core->main_core){
                long unsigned int node_id = core->main_core->node_id;
                mbind(ptr, size, MPOL_PREFERRED, &node_id, 3, 0);
        }
}

int ltg_malign(void **_ptr, size_t align, size_t size)
{
        int i;
        void *ptr=NULL;

        DBUG("mem %u\n", (int)size);
        
        /* Test whether the SIZE argument is valid.  It must be a power of
        two multiple of sizeof (void *).  */
        if (align % sizeof (void *) != 0
                || !powerof2 (align / sizeof (void *))
                || align == 0)
                return EINVAL;

        for (i = 0; i < 3; i++) {
                ptr = __memalign__(align, size);
                if (ptr != NULL) {
                        __ltg_malloc_bind(ptr, size);

                        *_ptr = ptr;
                        return 0;
                }
        }

        return ENOMEM;
}


int ltg_malloc(void **_ptr, size_t size)
{
        int ret, i;
        void *ptr = NULL;

        LTG_ASSERT(size != 0);

        if (unlikely(size == 0)) {
                *_ptr = NULL;
                return 0;
        }

        DBUG("mem %u\n", (int)size);

#if 0
        if (size < sizeof(struct list_head))
                size = sizeof(struct list_head);
#endif

        for (i = 0; i < 3; i++) {
                ptr = __calloc__(1, size);
                if (ptr != NULL) {
                        __ltg_malloc_bind(ptr, size);
                        
                        goto out;
                }
        }

        ret = ENOMEM;

        goto err_ret;

out:
        *_ptr = ptr;

        return 0;
err_ret:
        return ret;
}

void *ltg_malloc1(size_t size)
{
        void *ptr = malloc(size);

        DBUG("mem %u\n", (int)size);
        
        if (ptr) {
                __ltg_malloc_bind(ptr, size);
        }

        return ptr;
}


inline int ltg_realloc(void **_ptr, size_t size, size_t newsize)
{
        int ret, i;
        void *ptr;

        DBUG("mem %u\n", (int)size);
        
        if (*_ptr == NULL && size == 0) /*malloc*/ {
                ret = ltg_malloc(&ptr, newsize);
                if (ret)
                        GOTO(err_ret, ret);

                memset(ptr, 0x0, newsize);

                *_ptr = ptr;
                return 0;
        }

        if (newsize == 0)
                return ltg_free(_ptr);

        if (newsize < size) {
                ptr = *_ptr;
                memset(ptr + newsize, 0x0, size - newsize);
        }

#if 0
        if (newsize < sizeof(struct list_head))
                newsize = sizeof(struct list_head);
#endif

        ret = ENOMEM;
        for (i = 0; i < 3; i++) {
                ptr = realloc(*_ptr, newsize);
                if (ptr != NULL) {
                        __ltg_malloc_bind(ptr, newsize);
                        goto out;
                }
        }

        GOTO(err_ret, ret);
out:
        if (newsize > size)
                memset(ptr + size, 0x0, newsize - size);

        *_ptr = ptr;

        return 0;
err_ret:
        return ret;
}

int ltg_free(void **ptr)
{
        if (*ptr != NULL) {
                __free__(*ptr);
        } else {
                LTG_ASSERT(0);
        }

        *ptr = NULL;

        return 0;
}

void ltg_free1(void *ptr)
{
        free(ptr);
}

int huge_mem_alloc1(void **_ptr, size_t size)
{
        return ltg_malloc(_ptr, size);
}

void huge_mem_free1(void **ptr)
{
        ltg_free(ptr);
}


int huge_mem_realloc1(void **_ptr, size_t size, size_t newsize)
{
        return ltg_realloc(_ptr, size, newsize);
}

void *huge_mem_alloc(size_t size)
{
        int ret;
        void *ptr;
        
        ret = ltg_malloc(&ptr, size);
        if (ret)
                return NULL;
        else
                return ptr;
}

void huge_mem_free(void *ptr)
{
        ltg_free(&ptr);
}
