#include <numaif.h>
#include <sys/mman.h>

#define DBG_SUBSYS S_LTG_MEM

#include "ltg_utils.h"
#include "ltg_mem.h"
#include "core/core.h"

#define ENABLE_RING_TRACE 1

#define HPAGE_DUMP(hpage, head, name) do {      \
        if (head) LTG_ASSERT(hpage->head == head); \
        DBUG("name %s hpage %p head %p ref %d\n", \
              name, \
              hpage, \
              hpage->head, \
              hpage->ref); \
} while(0)

typedef enum {
        LINKED = 2,
        DELETE,
} page_status;

typedef struct {
        struct list_head list;
        void     *vaddr;
        int      ref;
        int      offset;
        void     *head;
        uint32_t  size;
        page_status status;
} mem_ring_t;

#define MEM_RING_DUMP_L(LEVEL, mr, format, a...) do { \
    LEVEL("ring %p head %p vaddr %p size %u offset %d ref %d status %d "format, \
          (mr),  \
          (mr)->head,  \
          (mr)->vaddr,  \
          (mr)->size,  \
          (mr)->offset,  \
          (mr)->ref,  \
          (mr)->status,  \
          ##a \
          ); \
} while(0)

#define MEM_RING_DUMP(mr, format, a...) MEM_RING_DUMP_L(DBUG, mr, format, ##a)

typedef struct {
        struct list_head free_list;
        struct list_head used_list;
        int nr_free;
        int nr_used;

        int nr_hugepage;
        int nr_alloc;

        uint64_t nr_bytes;

        time_t time;
        ltg_spinlock_t   lock;
        int              hash;
} mem_ring_head_t;

#define MEM_RING_HEAD_DUMP_L(LEVEL, head, format, a...) do { \
    LEVEL("mem_ring head %p hash %d hugepage %d/%d free %d used %d bytes %d/%d "format, \
          (head),  \
          (head)->hash,  \
          (head)->nr_hugepage,  \
          (head)->nr_alloc,  \
          (head)->nr_free,  \
          (head)->nr_used,  \
          (head)->nr_bytes / HUGEPAGE_SIZE,  \
          (head)->nr_bytes % HUGEPAGE_SIZE,  \
          ##a \
          ); \
} while(0)

#define MEM_RING_HEAD_DUMP(head, format, a...) MEM_RING_HEAD_DUMP_L(DBUG, head, format, ##a)

static mem_ring_head_t *__mem_ring__;
static __thread mem_ring_head_t *__mem_ring_private__ = NULL;

static inline void __mem_ring_dump(mem_ring_head_t *head)
{
        struct list_head *pos, *n;
        mem_ring_t *hpage = NULL;

        list_for_each_safe(pos, n, &head->free_list){
                hpage = list_entry(pos, mem_ring_t, list);
                MEM_RING_DUMP_L(DINFO, hpage, "\n");
        }

        list_for_each_safe(pos, n, &head->used_list){
                hpage = list_entry(pos, mem_ring_t, list);
                MEM_RING_DUMP_L(DINFO, hpage, "\n");
        }

        MEM_RING_HEAD_DUMP_L(DINFO, head, "\n");
}

static int __mem_ring_new__(mem_ring_head_t *head, mem_ring_t **_hpage, uint32_t *size)
{
        int ret;
        mem_ring_t *hpage;
        void *vaddr;
        uint32_t new_size;

        new_size = _align_up(*size, MAX_ALLOC_SIZE);

        ret = huge_mem_alloc1((void **)&hpage, sizeof(*hpage));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = hugepage_getfree((void **)&vaddr, &new_size, __FUNCTION__);
        if (unlikely(ret))
                GOTO(err_free, ret);

        hpage->vaddr = vaddr;
        hpage->ref = 0;
        hpage->offset = 0;
        hpage->status = LINKED;
        hpage->size = new_size;
        hpage->head = head;

        list_add_tail(&hpage->list, &head->free_list);
        head->nr_free++;

        head->nr_alloc += new_size / HUGEPAGE_SIZE;

        *_hpage = hpage;
        *size = new_size;

        MEM_RING_DUMP_L(DINFO, hpage, "\n");

        return 0;
err_free:
        huge_mem_free1((void **)&hpage);
err_ret:
        return ret;
}

static inline mem_ring_t *__hpage_list_find(mem_ring_head_t *head, uint32_t alloc_size)
{
        struct list_head *pos;
        mem_ring_t *hpage = NULL;

        list_for_each(pos, &head->free_list){
                hpage = list_entry(pos, mem_ring_t, list);
                if (hpage->offset + alloc_size <= hpage->size)
                        return hpage;
        }

        return NULL;
}

static inline int __mem_ring_mark_delete(mem_ring_head_t *head, mem_ring_t *hpage)
{
        LTG_ASSERT(hpage->ref);

        MEM_RING_DUMP_L(DBUG, hpage, "free %d used %d\n",
                        head->nr_free, head->nr_used);

        list_del(&hpage->list);
        head->nr_free--;

        list_add_tail(&hpage->list, &head->used_list);
        head->nr_used++;

#if ENABLE_RING_TRACE
        head->time = gettime();
#endif

        hpage->status = DELETE;

        return 0;
}

static inline int __mem_ring_new(mem_ring_head_t *head, uint32_t *size,
                                 mem_handler_t *mem_handler)
{
        int ret, new_page = 0;
        mem_ring_t *hpage;

        uint32_t alloc_size = _align_up(*size, PAGE_SIZE);
        if (alloc_size > MAX_ALLOC_SIZE)
                alloc_size = MAX_ALLOC_SIZE;

retry:
        if (unlikely(list_empty(&head->free_list) || new_page)){
                uint32_t new_size = alloc_size;
                ret = __mem_ring_new__(head, &hpage, &new_size);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                MEM_RING_HEAD_DUMP_L(DINFO, head, "new_page %d alloc %u size %u\n",
                                     new_page, alloc_size, new_size);

                if (alloc_size > new_size) {
                        alloc_size = new_size;
                }
        } else {
                hpage = list_entry(head->free_list.next, mem_ring_t, list);

                if (hpage->ref == 0) {
                        LTG_ASSERT(hpage->offset == 0);
                }

                if (alloc_size > hpage->size) {
                        hpage = __hpage_list_find(head, alloc_size);
                        if (hpage == NULL) {
                                new_page = 1;
                                goto retry;
                        }

                } else if (unlikely(hpage->offset + alloc_size > hpage->size)) {
                        __mem_ring_mark_delete(head, hpage);

                        goto retry;
                }
        }

        LTG_ASSERT(alloc_size % PAGE_SIZE == 0);

        mem_handler->head = hpage;
        mem_handler->pool = head;
        mem_handler->ptr = hpage->vaddr + hpage->offset;

        hpage->ref++;
        hpage->offset += alloc_size;

        head->nr_bytes += alloc_size;

        // MEM_RING_DUMP_L(DBUG, hpage, "alloc %u\n", alloc_size);
        // MEM_RING_HEAD_DUMP_L(DBUG, head, "alloc %u\n", alloc_size);

        if (hpage->offset + PAGE_SIZE > (int)hpage->size) {
                __mem_ring_mark_delete(head, hpage);
        }

#if 0
        if (new_page) {
                __mem_ring_dump(head);
        }
#endif

        *size = alloc_size;

        return 0;
err_ret:
        return ret;
}

int mem_ring_new(uint32_t *size, mem_handler_t *mem_handler)
{
        int ret;
        mem_ring_head_t *head;
        core_t *core = core_self();

        if (unlikely(__mem_ring_private__ == NULL)) {
public:
                head = __mem_ring__;
                ret = ltg_spin_lock(&head->lock);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                ret = __mem_ring_new(head, size, mem_handler);
                if (unlikely(ret)) {
                        UNIMPLEMENTED(__DUMP__);
                        GOTO(err_lock, ret);
                }

                ltg_spin_unlock(&head->lock);
        } else {
                head = __mem_ring_private__;
                LTG_ASSERT(core->hash == head->hash);

                ret = __mem_ring_new(head, size, mem_handler);
                if (unlikely(ret)) {
                        DINFO("%s[%s] no mem, use public\n", core->name, core->hash);
                        goto public;
                }

        }

        return 0;
err_lock:
        ltg_spin_unlock(&head->lock);
err_ret:
        return ret;
}

static void __mem_ring_local_free(mem_handler_t *mem_handler)
{
        mem_ring_head_t *head = mem_handler->pool;
        mem_ring_t *hpage = mem_handler->head;

        MEM_RING_DUMP_L(DBUG, hpage, "\n");

        LTG_ASSERT(hpage->ref > 0);
        hpage->ref--;

        HPAGE_DUMP(hpage, head, __FUNCTION__);

        if (hpage->ref == 0) {
                // in free list
                head->nr_bytes -= hpage->offset;
                // MEM_RING_HEAD_DUMP_L(DBUG, head, "free %u\n", hpage->offset);

                hpage->offset = 0;
        }

        if (hpage->ref == 0 && hpage->status == DELETE) {
                // in used list
                MEM_RING_DUMP_L(DBUG, hpage, "free %u used %u\n",
                                head->nr_free, head->nr_used);

                hpage->offset = 0;
                hpage->status = LINKED;

                list_del(&hpage->list);
                head->nr_used--;

                list_add_tail(&hpage->list, &head->free_list);
                head->nr_free++;

#if ENABLE_RING_TRACE
                head->time = gettime();
#endif
        }
}

static int __cross_free_va(va_list ap)
{
        mem_handler_t mem_handler = va_arg(ap, mem_handler_t);
        va_end(ap);

        DBUG("cross free %p %p\n", mem_handler.pool, mem_handler.head);

        __mem_ring_local_free(&mem_handler);

        return 0;
}

static void __mem_ring_crossfree(core_t *core, mem_handler_t *mem_handler)
{
        mem_ring_head_t *head = mem_handler->pool;

        DBUG("cross free %p %p\n", mem_handler->pool, mem_handler->head);

        (void) core;
        int ret = core_request(head->hash, -1, "mem_ring_deref",
                               __cross_free_va, *mem_handler);
        LTG_ASSERT(ret == 0);
}

void mem_ring_deref(mem_handler_t *mem_handler)
{
        mem_ring_head_t *head = mem_handler->pool;
        core_t *core = core_self();

        if (likely(core && (core->hash == head->hash))) {
                __mem_ring_local_free(mem_handler);
        } else if (head == __mem_ring__) {
                int ret = ltg_spin_lock(&head->lock);
                if (ret) {
                        UNIMPLEMENTED(__DUMP__);
                }

                __mem_ring_local_free(mem_handler);

                ltg_spin_unlock(&head->lock);
        } else {
                DBUG("cross free\n");

                __mem_ring_crossfree(core, mem_handler);
        }
}

void mem_ring_check(const mem_handler_t *mem_handler)
{
        mem_ring_t *hpage = mem_handler->head;

        LTG_ASSERT(hpage->ref > 0);
}

int mem_ring_ref(mem_handler_t *mem_handler)
{
        int ret;
        mem_ring_head_t *head = mem_handler->pool;
        mem_ring_t *hpage = mem_handler->head;

        if (unlikely(head == __mem_ring__)) {
                ret = ltg_spin_lock(&head->lock);
                if (ret) {
                        GOTO(err_ret, ret);
                }
        } else {
#if 0
                core_t *core = core_self();
                if (core->hash != head->hash) {
                        LTG_ASSERT(0);
                }
#endif
        }

        hpage->ref++;

        if (unlikely(head == __mem_ring__)) {
                ltg_spin_unlock(&head->lock);
        }

        return 0;
err_ret:
        return ret;
}

static int __mem_ring_head_init(mem_ring_head_t *head)
{
        int ret;

        hugepage_show(__FUNCTION__);

        memset(head, 0x00, sizeof(*head));

        INIT_LIST_HEAD(&head->free_list);
        INIT_LIST_HEAD(&head->used_list);

        head->nr_used = 0;
        head->nr_free = 0;

        head->nr_hugepage = hugepage_get();
        head->nr_alloc = 1;

        head->nr_bytes = HUGEPAGE_SIZE;

        ret = ltg_spin_init(&head->lock);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int mem_ring_init()
{
        int ret;
        mem_ring_head_t *head;
        uint32_t size = HUGEPAGE_SIZE;

        ret = hugepage_getfree((void **)&head, &size, __FUNCTION__);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        __mem_ring_head_init(head);

        head->hash = -1;

        __mem_ring__ = head;

        return 0;
err_ret:
        return ret;
}

static void __mem_ring_scan__(core_t *core, mem_ring_head_t *head)
{
        struct list_head *pos;
        mem_ring_t *hpage;
        time_t now = gettime();

        uint32_t seq = 0;
        list_for_each(pos, &head->used_list) {
                hpage = (void *)pos;

                if (now - head->time > 256) {
                        DWARN("%s[%d] ring %p used %u free %u,"
                              " page %p ref %d, status %d, seq[%d],"
                              " last update %d %d, offset %u\n",
                              core->name, core->hash, head, head->nr_used, head->nr_free,
                              hpage, hpage->ref, hpage->status, seq,
                              now - head->time, head->time, hpage->offset);
                        seq++;
                }
        }
}


static inline void __mem_ring_scan(void *_core, void *var, void *_head)
{
        (void) _core;
        (void) var;

        mem_ring_head_t *head = _head;

        if (head->nr_used) {
                __mem_ring_scan__(_core, head);
        }

        return;
}

int mem_ring_private_init(int corehash)
{
        int ret;
        mem_ring_head_t *head;
        uint32_t size = HUGEPAGE_SIZE;

        ret = hugepage_getfree((void **)&head, &size, __FUNCTION__);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        __mem_ring_head_init(head);

        head->hash = corehash;

        __mem_ring_private__ = head;

        ret = core_register_scan("mem_ring_scan", __mem_ring_scan, head);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}
