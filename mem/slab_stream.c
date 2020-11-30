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
#define SLAB_MAGIC 0x347acaf

int slab_stream_init()
{
        int ret;

        ret = slab_init("stream", &__slab_public__, &__slab_array_public__,
                        MEM_MIN, MEM_SHIFT, SLAB_MAGIC);
        if (ret)
                GOTO(err_ret, ret);
        
        return 0;
err_ret:
        return ret;
}

inline static void __slab_scan(void *_core, void *var, void *_slab)
{
        (void) _core;
        (void) var;

        slab_t *slab = _slab;

        slab_scan(_core, slab->private);
        
        return;
}

int slab_stream_private_init()
{
        int ret;
        slab_t *slab;

        ret = slab_private_init("stream", &slab, __slab_array_public__,
                                MEM_MIN, MEM_SHIFT, SLAB_MAGIC);
        if (ret)
                GOTO(err_ret, ret);

        core_tls_set(VARIABLE_SLAB_STREAM, slab);

#if 1
        ret = core_register_scan("slab_stream_scan", __slab_scan, slab);
        if (ret)
                GOTO(err_ret, ret);
#endif
        
        return 0;
err_ret:
        return ret;
}

void S_LTG *slab_stream_alloc(size_t size)
{
        slab_t *slab = core_tls_get(NULL, VARIABLE_SLAB_STREAM);

        if (unlikely(slab == NULL)) {
                slab = __slab_public__;
        }
        
        return slab_alloc(slab, size);
}

void S_LTG slab_stream_free(void *ptr)
{
        slab_t *slab = core_tls_get(NULL, VARIABLE_SLAB_STREAM);

        if (unlikely(slab == NULL)) {
                slab = __slab_public__;
        }
        
        slab_free(slab, ptr);
}

void S_LTG *slab_stream_alloc_glob(size_t size)
{
        return slab_alloc(__slab_public__, size);
}

int S_LTG slab_stream_alloc1(void **_ptr, size_t size)
{
        void *ptr = slab_stream_alloc(size);
        if (ptr == NULL) {
                return ENOMEM;
        } else {
                *_ptr = ptr;
                return 0;
        }
}

void S_LTG slab_stream_free1(void **ptr)
{
        if (*ptr) {
                slab_stream_free(*ptr);
                *ptr = NULL;
        }
}
