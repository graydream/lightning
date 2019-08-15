#include <sys/types.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_MEM

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_core.h"
#include "mem/slab.h"

static slab_array_t *__slab_array_public__;
static slab_t *__slab_public__;

#define MEM_MIN 64
#define MEM_SHIFT (14)
#define SLAB_MAGIC 0x347a84d


int slab_static_init()
{
        int ret;

        ret = slab_init("static", &__slab_public__, &__slab_array_public__,
                        MEM_MIN, MEM_SHIFT, SLAB_MAGIC);
        if (ret)
                GOTO(err_ret, ret);
        
        return 0;
err_ret:
        return ret;
}

int slab_static_private_init()
{
        int ret;
        slab_t *slab;

        ret = slab_private_init("static", &slab, __slab_array_public__,
                                MEM_MIN, MEM_SHIFT, SLAB_MAGIC);
        if (ret)
                GOTO(err_ret, ret);

        __core_tls_set(VARIABLE_SLAB_STATIC, slab);
        
        return 0;
err_ret:
        return ret;
}

void IO_FUNC *slab_static_alloc(size_t size)
{
        slab_t *slab = __core_tls_get(NULL, VARIABLE_SLAB_STATIC);

        if (unlikely(slab == NULL)) {
                slab = __slab_public__;
        }
        
        return slab_alloc(slab, size);
}

void IO_FUNC slab_static_free(void *ptr)
{
        slab_t *slab = __core_tls_get(NULL, VARIABLE_SLAB_STATIC);

        if (unlikely(slab == NULL)) {
                slab = __slab_public__;
        }
        
        slab_free(slab, ptr);
}

void IO_FUNC *slab_static_alloc_glob(size_t size)
{
        return slab_alloc(__slab_public__, size);
}

int IO_FUNC slab_static_alloc1(void **_ptr, size_t size)
{
        void *ptr = slab_static_alloc(size);
        if (ptr == NULL) {
                return ENOMEM;
        } else {
                *_ptr = ptr;
                return 0;
        }
}

void IO_FUNC slab_static_free1(void **ptr)
{
        if (*ptr) {
                slab_static_free(*ptr);
                *ptr = NULL;
        }
}
