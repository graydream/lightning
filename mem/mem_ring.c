#include <numaif.h>
#include <sys/mman.h>

#define DBG_SUBSYS S_LTG_MEM

#include "ltg_utils.h"
#include "ltg_mem.h"
#include "core/core.h"

#define ENABLE_RING_TRACE 1

typedef struct {
        struct list_head used_list;
        int nr_used;

        hpage_t *cur_hpage[MAX_SIZE];

        time_t time;
        int hash;
        int numa;
        void *hpage_list;
        ltg_spinlock_t   lock;
} mem_ring_head_t;

/*#define MEM_RING_HEAD_DUMP_L(LEVEL, head, format, a...) do { \
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
*/
static mem_ring_head_t *__mem_ring__;
static __thread mem_ring_head_t *__mem_ring_private__ = NULL;


inline static int INLINE __mem_ring_new(mem_ring_head_t *head, uint32_t *size,
                                 mem_handler_t *mem_handler)
{
        int ret;
        uint32_t alloc_size = _align_up(*size, PAGE_SIZE);
        int offset = (alloc_size - 1) / HUGEPAGE_SIZE;
        
        if (unlikely(offset >= MAX_SIZE)) {
                offset = MAX_SIZE - 1;
                alloc_size =  HUGEPAGE_SIZE * MAX_SIZE;
        }

        hpage_t *hpage = head->cur_hpage[offset];
        
        if ((hpage && hpage->offset + alloc_size > hpage->size) || hpage == NULL) {

                hpage = hpage_get(head->hpage_list, offset);
                if (unlikely(hpage == NULL)) {
                        ret = ENOMEM;
                        GOTO(err_ret, ret);
                }

                list_add_tail(&hpage->list, &head->used_list);
                head->nr_used++;
                head->cur_hpage[offset] = hpage;
        } 
 
        LTG_ASSERT(alloc_size % PAGE_SIZE == 0);

        mem_handler->head = hpage;
        mem_handler->pool = head;
        mem_handler->ptr = hpage->vaddr + hpage->offset;

        hpage->ref++;
        hpage->offset += alloc_size;

        *size = alloc_size;

        return 0;
err_ret:
        return ret;
}

inline int INLINE mem_ring_new(uint32_t *size, mem_handler_t *mem_handler)
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
        hpage_t *hpage = mem_handler->head;

        LTG_ASSERT(hpage->ref > 0);
        hpage->ref--;

        if (hpage->ref == 0) {
                head->nr_used--;
                hpage->offset = 0;

                LTG_ASSERT(((hpage->type + 1)* HUGEPAGE_SIZE) == hpage->size);
                if (head->cur_hpage[hpage->type] != hpage){
                        list_del(&hpage->list);
                        hpage_free(head->hpage_list, (hpage->size - 1)/ HUGEPAGE_SIZE, hpage);
                }

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
                UNIMPLEMENTED(__DUMP__);
                DBUG("cross free\n");

                __mem_ring_crossfree(core, mem_handler);
        }
}

void mem_ring_check(const mem_handler_t *mem_handler)
{
        hpage_t *hpage = mem_handler->head;

        LTG_ASSERT(hpage->ref > 0);
}

int mem_ring_ref(mem_handler_t *mem_handler)
{
        int ret;
        mem_ring_head_t *head = mem_handler->pool;
        hpage_t *hpage = mem_handler->head;

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

        memset(head, 0x00, sizeof(*head));

        INIT_LIST_HEAD(&head->used_list);

        head->nr_used = 0;

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
        void *ptr;
        uint32_t size = HUGEPAGE_SIZE;

        hugepage_init(&ptr);
        ret = hugepage_getfree((void **)&head, &size, __FUNCTION__);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        __mem_ring_head_init(head);

        head->hpage_list = ptr;
        head->hash = -1;

        __mem_ring__ = head;

        return 0;
err_ret:
        return ret;
}

static void __mem_ring_scan__(core_t *core, mem_ring_head_t *head)
{
        struct list_head *pos;
        hpage_t *hpage;
        time_t now = gettime();

        uint32_t seq = 0;
        list_for_each(pos, &head->used_list) {
                hpage = (void *)pos;

                if (now - head->time > 256) {
                        DWARN("%s[%d] ring %p used %d ,"
                              " page %p ref %d, seq[%d],"
                              " last update %d %d, offset %u\n",
                              core->name, core->hash, head, head->nr_used,
                              hpage, hpage->ref,  seq,
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
        void *ptr;
        mem_ring_head_t *head;
        core_t *core = core_self();
        uint32_t size = HUGEPAGE_SIZE;
        
        if (core->main_core)
                private_hugepage_init(core->main_core->node_id, &ptr);

        ret = hugepage_getfree((void **)&head, &size, __FUNCTION__);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        __mem_ring_head_init(head);

        head->hpage_list = ptr;
        head->hash = corehash;

        if (core->main_core)
                head->numa = core->main_core->node_id;
        __mem_ring_private__ = head;

        ret = core_register_scan("mem_ring_scan", __mem_ring_scan, head);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}
