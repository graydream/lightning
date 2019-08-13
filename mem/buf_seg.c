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

#if 0
#define DBT(format, a...)                                               \
        do {                                                            \
                char __bt__[MAX_BUF_LEN];                               \
                calltrace(__bt__, MAX_BUF_LEN);                         \
                D_MSG(__D_INFO, "DBT: " format " backtrace : %s\n", ## a, __bt__); \
        } while (0)

#else
#define DBT(format, a...)
#endif

/*shared*/
static void __seg_free_head(seg_t *seg, int sys)
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

static seg_t *__seg_alloc_head(ltgbuf_t *buf, uint32_t size, int sys)
{
        seg_t *seg;

        LTG_ASSERT(size <= BUFFER_SEG_SIZE);

        if (likely(buf->used < SEG_KEEP)) {
                seg = &buf->array[buf->used];
                seg->local = 1;
                buf->used++;
        } else {
                if (likely(!sys)) {
                        seg = slab_stream_alloc(sizeof(seg_t));
                        LTG_ASSERT(seg);
                        if (ltgconf.rdma) {
                                DINFO("%p alloc %p, len %u\n", buf, seg, size);
                        }
                } else {
                        int ret = ltg_malloc((void **)&seg, sizeof(seg_t));
                        LTG_ASSERT(ret == 0);
                }

                seg->local = 0;
        }

        seg->len = size;
        seg->shared = 0;
        seg->huge.head = NULL;
        seg->handler.ptr = NULL;
        seg->sop.seg_share = NULL;
        seg->sop.seg_free = NULL;
        seg->sop.seg_trans = NULL;

        DBUG("ptr %p %u %u %p\n", seg->handler.ptr, seg->len, size, seg->huge.head);
        LTG_ASSERT(seg->len == size && seg->huge.head == NULL);

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

        ret = posix_memalign((void **)&seg->sys.base, PAGE_SIZE, seg->len);
        if (unlikely(ret)) {
                GOTO(err_free, ret);
        }

        DBT("alloc %p", seg->sys.base);
        
        seg->handler.ptr = seg->sys.base;
        seg->handler.phyaddr = 0;
        
        seg->sop.seg_free = __seg_sys_free;
        seg->sop.seg_share = __seg_sys_share;
        seg->sop.seg_trans = __seg_sys_trans;
        
        return seg;
err_free:
        UNIMPLEMENTED(__DUMP__);
        return NULL;
}

/*seg ext object*/

static void __seg_ext_free(seg_t *seg)
{
        seg->ext.cb(seg->ext.arg);

        __seg_free_head(seg, 0);
}

static seg_t *__seg_ext_share(ltgbuf_t *buf, seg_t *src)
{
        seg_t *seg;

        seg = __seg_alloc_head(buf, src->len, 0);
        seg->sop = src->sop;
        seg->ext = src->ext;
        seg->handler = src->handler;

        return seg;
}

static seg_t *__seg_ext_trans(ltgbuf_t *buf, seg_t *seg)
{
        seg_t *newseg = __seg_alloc_head(buf, seg->len, 0);

        newseg->handler = seg->handler;
        newseg->sop = seg->sop;
        newseg->ext = seg->ext;

        __seg_free_head(seg, 0);

        return newseg;
}

inline seg_t *seg_ext_create(ltgbuf_t *buf, void *data, uint32_t size,
                             void *arg, int (*cb)(void *arg))
{
        seg_t *seg;

        seg = __seg_alloc_head(buf, size, 0);
        seg->sop.seg_free = __seg_ext_free;
        seg->sop.seg_share = __seg_ext_share;
        seg->sop.seg_trans = __seg_ext_trans;
        
        seg->handler.ptr = data;
        seg->handler.phyaddr = 0;

        seg->ext.cb = cb;
        seg->ext.arg = arg;

        return seg;
}

/*seg hugepage object*/

#if ENABLE_HUGEPAGE

static void __seg_huge_free(seg_t *seg)
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

static seg_t *__seg_huge_share(ltgbuf_t *buf, seg_t *src)
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

inline seg_t *seg_huge_create(ltgbuf_t *buf, uint32_t size)
{
        int ret;
        seg_t *seg;

        seg = __seg_alloc_head(buf, size, 0);
        if (unlikely(!seg))
                return NULL;

        mem_handler_t handler;
        ret = mem_ring_new(size, &handler);
        if (unlikely(ret)) {
                GOTO(err_free, ret);
        }

        seg->huge.pool = handler.pool;
        seg->huge.head = handler.head;
        seg->handler.ptr = handler.ptr;
        seg->handler.phyaddr = handler.phyaddr;
                
        DBUG("ptr %p %u\n", seg->handler.ptr, seg->len);
        LTG_ASSERT(seg->handler.ptr);
        LTG_ASSERT(seg->huge.head);

        seg->sop.seg_free = __seg_huge_free;
        seg->sop.seg_share = __seg_huge_share;
        seg->sop.seg_trans = __seg_huge_trans;
        
        return seg;
err_free:
        UNIMPLEMENTED(__DUMP__);
        return NULL;
}

#else

inline seg_t *seg_huge_create(ltgbuf_t *buf, uint32_t size)
{
        return seg_sys_create(buf, size);
}

static seg_t *__seg_huge_share(ltgbuf_t *buf, seg_t *src)
{
        return __seg_sys_share(buf, src);
}

#endif

inline void seg_add_tail(ltgbuf_t *buf, seg_t *seg)
{
        LTG_ASSERT(seg->len);
        list_add_tail(&seg->hook, &buf->list);
        buf->len += seg->len;
}

inline void seg_check(seg_t *seg)
{
        if (seg->sop.seg_share == __seg_huge_share) {
                mem_handler_t handler;

                handler.pool = seg->huge.pool;
                handler.head = seg->huge.head;
                handler.ptr = seg->handler.ptr;
                handler.phyaddr = seg->handler.phyaddr;
                mem_ring_check(&handler);
        }
}
