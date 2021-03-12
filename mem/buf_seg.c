#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>

#define DBG_SUBSYS S_LTG_MEM

#include "ltg_utils.h"
#include "core/sche.h"

static seg_ops_t __sop_huge__;
static seg_ops_t __sop_sys__;
static seg_ops_t __sop_ext__;
static seg_ops_t __sop_solid__;

/*shared*/
static void S_LTG __seg_free_head(seg_t *seg, int sys)
{
        if (unlikely(seg->local == 0)) {
                DBUG("free %p, local %d\n", seg, seg->local);

                if (likely(!sys)) {
                        slab_stream_free(seg);
                } else {
                        ltg_free((void **)&seg);
                }
        }
}

static seg_t S_LTG *__seg_alloc_head(ltgbuf_t *buf, uint32_t size, int sys)
{
        seg_t *seg;

        if (likely(buf->used < SEG_KEEP)) {
                seg = &buf->array[buf->used];
                //memset(seg, 0x0, sizeof(*seg));
                seg->local = 1;
                buf->used++;
        } else {
                if (likely(!sys)) {
                        seg = slab_stream_alloc(sizeof(seg_t));
                        LTG_ASSERT(seg);
                        DBUG("%p alloc %p, len %u\n", buf, seg, size);
                } else {
                        int ret = ltg_malloc((void **)&seg, sizeof(seg_t));
                        if (unlikely(ret)) {
                                DERROR("malloc fail\n");
                                UNIMPLEMENTED(__DUMP__);
                        }
                }

                //memset(seg, 0x0, sizeof(*seg));
                seg->local = 0;
        }

        seg->len = size;
        seg->shared = 0;

        return seg;
}

/*seg system modules*/

static void __seg_sys_free(seg_t *seg)
{
        if (seg->shared == 0) {
                DBT("free %p", seg->sys.base);
                ltg_free((void **)&seg->sys.base);
        } else {
                DBT("nofree %p", seg->sys.base);
        }

        __seg_free_head(seg, 1);
}

static seg_t *__seg_sys_share(ltgbuf_t *buf, seg_t *src)
{
        seg_t *newseg;

        newseg = __seg_alloc_head(buf, src->len, 1);
        if (!newseg)
                return NULL;

        DBT("share %p", src->sys.base);
        
        newseg->handler = src->handler;
        newseg->sop = src->sop;
        newseg->sys = src->sys;
        newseg->shared = 1;

        return newseg;
}

static seg_t *__seg_sys_trans(ltgbuf_t *buf, seg_t *seg)
{
        seg_t *newseg = __seg_alloc_head(buf, seg->len, 1);

        newseg->handler = seg->handler;
        newseg->sop = seg->sop;
        newseg->sys = seg->sys;
        newseg->shared = seg->shared;

        DBT("share %p", seg->sys.base);
        
        __seg_free_head(seg, 1);

        return newseg;
}

inline seg_t *seg_sys_create(ltgbuf_t *buf, uint32_t size)
{
        int ret;
        seg_t *seg;

        seg = __seg_alloc_head(buf, size, 1);
        if (unlikely(!seg))
                return NULL;

        if (seg->len % 512 == 0) {
                ret = ltg_malign((void **)&seg->sys.base, 512, seg->len);
                if (unlikely(ret)) {
                        DERROR("malloc fail, size %u %u\n", size, seg->len);
                        UNIMPLEMENTED(__DUMP__);
                        GOTO(err_free, ret);
                }
        } else {
                ret = ltg_malloc((void **)&seg->sys.base, seg->len);
                if (unlikely(ret)) {
                        DERROR("malloc fail, size %u %u\n", size, seg->len);
                        UNIMPLEMENTED(__DUMP__);
                        GOTO(err_free, ret);
                }
        }

        DBT("alloc %p", seg->sys.base);

        seg->handler.ptr = seg->sys.base;
        seg->handler.phyaddr = 0;
        
        seg->sop = &__sop_sys__;
        
        return seg;
err_free:
        UNIMPLEMENTED(__DUMP__);
        return NULL;
}

/*seg ext object*/

static void S_LTG __seg_ext_free(seg_t *seg)
{
        if (seg->ext.cb) {
                seg->ext.cb(seg->ext.arg);
        }

        __seg_free_head(seg, 0);
}

static seg_t S_LTG *__seg_ext_share(ltgbuf_t *buf, seg_t *src)
{
        seg_t *seg;

        seg = __seg_alloc_head(buf, src->len, 0);
        seg->sop = src->sop;
        seg->ext = src->ext;
        seg->handler = src->handler;

        return seg;
}

static seg_t S_LTG *__seg_ext_trans(ltgbuf_t *buf, seg_t *seg)
{
        seg_t *newseg = __seg_alloc_head(buf, seg->len, 0);

        newseg->handler = seg->handler;
        newseg->sop = seg->sop;
        newseg->ext = seg->ext;

        __seg_free_head(seg, 0);

        return newseg;
}

seg_t S_LTG *seg_ext_create(ltgbuf_t *buf, void *data, uint32_t size,
                             void *arg, int (*cb)(void *arg))
{
        seg_t *seg;

        seg = __seg_alloc_head(buf, size, 0);

        seg->sop = &__sop_ext__;
        
        seg->handler.ptr = data;
        seg->handler.phyaddr = 0;

        seg->ext.cb = cb;
        seg->ext.arg = arg;

        return seg;
}

/*seg hugepage object*/

#if ENABLE_HUGEPAGE

static void S_LTG __seg_huge_free(seg_t *seg)
{
        LTG_ASSERT(seg->huge.head);

        if (seg->shared == 0) {
                mem_handler_t handler;

                handler.pool = seg->huge.pool;
                handler.head = seg->huge.head;
                handler.ptr = seg->handler.ptr;
                handler.phyaddr = seg->handler.phyaddr;
                mem_ring_deref(&handler);
        }

        __seg_free_head(seg, 0);
}

inline static seg_t INLINE *__seg_huge_share(ltgbuf_t *buf, seg_t *src)
{
        seg_t *newseg;

        newseg = __seg_alloc_head(buf, src->len, 0);
        if (!newseg)
                return NULL;

        newseg->handler = src->handler;
        newseg->sop = src->sop;
        newseg->huge = src->huge;
        newseg->shared = 1;
        LTG_ASSERT(newseg->huge.head);

        return newseg;
}

static seg_t *__seg_huge_trans(ltgbuf_t *buf, seg_t *seg)
{
        seg_t *newseg = __seg_alloc_head(buf, seg->len, 0);

        newseg->handler = seg->handler;
        newseg->sop = seg->sop;
        newseg->shared = seg->shared;
        newseg->huge = seg->huge;

        __seg_free_head(seg, 0);

        return newseg;
}

inline seg_t INLINE *seg_huge_create(ltgbuf_t *buf, uint32_t *size)
{
        int ret;
        uint32_t newsize = *size;
        seg_t *seg;

        seg = __seg_alloc_head(buf, *size, 0);
        if (unlikely(!seg))
                return NULL;

        mem_handler_t handler;
        ret = mem_ring_new(&newsize, &handler);
        if (unlikely(ret)) {
                GOTO(err_free, ret);
        }

        if (newsize < *size) {
                seg->len = newsize;
                *size = newsize;
        }

        seg->huge.pool = handler.pool;
        seg->huge.head = handler.head;
        seg->handler.ptr = handler.ptr;
                
        DBUG("ptr %p %u\n", seg->handler.ptr, seg->len);
        LTG_ASSERT(seg->handler.ptr);
        LTG_ASSERT(seg->huge.head);

        seg->sop = &__sop_huge__;
        
        return seg;
err_free:
        UNIMPLEMENTED(__DUMP__);
        return NULL;
}

#else

inline seg_t INLINE *seg_huge_create(ltgbuf_t *buf, uint32_t *size)
{
        return seg_sys_create(buf, *size);
}

static seg_t *__seg_huge_share(ltgbuf_t *buf, seg_t *src)
{
        return __seg_sys_share(buf, src);
}

#endif

inline void INLINE seg_add_tail(ltgbuf_t *buf, seg_t *seg)
{
        LTG_ASSERT(seg->len);
        list_add_tail(&seg->hook, &buf->list);
        buf->len += seg->len;
}

inline void seg_check(seg_t *seg)
{
        if (seg->sop->seg_share == __seg_huge_share) {
                mem_handler_t handler;

                handler.pool = seg->huge.pool;
                handler.head = seg->huge.head;
                handler.ptr = seg->handler.ptr;
                handler.phyaddr = seg->handler.phyaddr;
                mem_ring_check(&handler);
        }
}

/*seg solid modules*/

static void __seg_solid_free(seg_t *seg)
{
        if (seg->shared == 0) {
                DBT("free %p", seg->solid.base);
                slab_static_free1((void **)&seg->solid.base);
        } else {
                DBT("nofree %p", seg->solid.base);
        }

        __seg_free_head(seg, 1);
}

static seg_t *__seg_solid_share(ltgbuf_t *buf, seg_t *src)
{
        seg_t *newseg;

        newseg = __seg_alloc_head(buf, src->len, 1);
        if (!newseg)
                return NULL;

        DBT("share %p", src->solid.base);
        
        newseg->handler = src->handler;
        newseg->sop = src->sop;
        newseg->solid = src->solid;
        newseg->shared = 1;

        return newseg;
}

static seg_t *__seg_solid_trans(ltgbuf_t *buf, seg_t *seg)
{
        seg_t *newseg = __seg_alloc_head(buf, seg->len, 1);

        newseg->handler = seg->handler;
        newseg->sop = seg->sop;
        newseg->solid = seg->solid;
        newseg->shared = seg->shared;

        DBT("share %p", seg->solid.base);
        
        __seg_free_head(seg, 1);

        return newseg;
}

inline seg_t *seg_solid_create(ltgbuf_t *buf, uint32_t size)
{
        int ret;
        seg_t *seg;

        seg = __seg_alloc_head(buf, size, 1);
        if (unlikely(!seg))
                return NULL;

        ret = slab_static_alloc1((void **)&seg->solid.base, seg->len);
        if (unlikely(ret)) {
                GOTO(err_free, ret);
        }

        DBT("alloc %p", seg->solid.base);
        
        seg->handler.ptr = seg->solid.base;
        seg->handler.phyaddr = 0;
        
        seg->sop = &__sop_solid__;

        return seg;
err_free:
        UNIMPLEMENTED(__DUMP__);
        return NULL;
}

static void __seg_huge_init(seg_ops_t *sop)
{
        sop->seg_free = __seg_huge_free;
        sop->seg_share = __seg_huge_share;
        sop->seg_trans = __seg_huge_trans;
}

static void __seg_sys_init(seg_ops_t *sop)
{
        sop->seg_free = __seg_sys_free;
        sop->seg_share = __seg_sys_share;
        sop->seg_trans = __seg_sys_trans;
}

static void __seg_ext_init(seg_ops_t *sop)
{
        sop->seg_free = __seg_ext_free;
        sop->seg_share = __seg_ext_share;
        sop->seg_trans = __seg_ext_trans;
}

static void __seg_solid_init(seg_ops_t *sop)
{
        sop->seg_free = __seg_solid_free;
        sop->seg_share = __seg_solid_share;
        sop->seg_trans = __seg_solid_trans;
}

void seg_init()
{
        __seg_huge_init(&__sop_huge__);
        __seg_sys_init(&__sop_sys__);
        __seg_ext_init(&__sop_ext__);
        __seg_solid_init(&__sop_solid__);
}
