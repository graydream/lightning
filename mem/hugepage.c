#include <numaif.h>
#include <sys/mman.h>
#include <numa.h>

#define DBG_SUBSYS S_LTG_MEM

#include "mem/hugepage.h"
#include "ltg_utils.h"
#include "core/core.h"

struct list_head memseg_head[MAX_NUMA];
static hpage_head_t    __hp__;
static __thread  hpage_head_t  __private_hp__;
int use_huge = 1, next_numa = 1, numa_count;
uint32_t hugepage_size;

static int __map_hugepages(void *addr, int hp_count)
{
        int i, j, ret;
        char *map_ret;
        void *map_addr = addr;
        void *unmap_addr = addr;

        DINFO("hp_count %d\n", hp_count);

        for (i = 0; i < hp_count; i++) {
                map_ret = mmap(map_addr, hugepage_size, PROT_READ | PROT_WRITE,
                                MAP_HUGETLB|MAP_PRIVATE |MAP_FIXED|MAP_ANONYMOUS|0x40000 , -1, 0);
                if (map_ret == MAP_FAILED) {
                        DINFO("mmap erro  %p %d %s\n", map_addr, hp_count, strerror(errno))
                                ret = ENOMEM;
                        GOTO(err_ret, ret);
                }

                map_addr += hugepage_size;
        }

        return 0;

err_ret:
        for (j = 0; j < i; j++) {
                munmap(unmap_addr, hugepage_size);
                unmap_addr += hugepage_size;
        }
        return ret;
}

static void __slice_bind(memslice_t *slice, int numa)
{
        unsigned long int  sock;
        uint32_t bind_size;

        sock = 1 << numa;
        if (hugepage_size > MEMSLICE_SIZE) 
                bind_size = hugepage_size;
        else
                bind_size = MEMSLICE_SIZE;
        
        if (((uint64_t)slice->addr) % hugepage_size == 0) {
                mbind(slice->addr, bind_size, MPOL_PREFERRED, &sock, 3, 0);
                memset(slice->addr, 0x00, bind_size);
        }

        slice->map = 1;
}

inline static void* __memalign(void *ptr)
{
        size_t align;
        void *addr;

        align = (size_t)ptr & (HUGEPAGE_SIZE * 512 - 1);
        if (align){
                addr = ptr + HUGEPAGE_SIZE * 512 - align;
        }  else 
                addr = ptr;

        return addr;
}

inline static void __init_memseg_slice(memseg_t *memseg)
{
        memslice_t *slice;
        int i;

        LTG_ASSERT(memseg->size % MEMSLICE_SIZE == 0);
        
        for (i = 0; i < (int)(memseg->size / MEMSLICE_SIZE); i++) {
                slice = ltg_malloc1(sizeof(memslice_t));
                memset(slice, 0x00, sizeof(memslice_t));
                slice->addr = memseg->start_addr + ((size_t)i) * MEMSLICE_SIZE;
                slice->memseg = memseg;
                list_add_tail(&slice->list, &memseg->slice_list);
        }
}

inline static size_t __max_malloc_size(size_t memseg_size)
{
        size_t size = memseg_size;
        void *ptr = ltg_malloc1(size);
        int ret;

        while (ptr == NULL) {
                if (errno != ENOMEM) {
                        ret = errno;
                        GOTO(err_ret, ret);
                }

                size /= 2;
                if (size < MIN_MEMSEG_SIZE) {
                        ret = ENOMEM;
                        GOTO(err_ret, ret);
                }

                free(ptr);
                ptr = ltg_malloc1(size);
        }

        free(ptr);

        return size;

err_ret:
        return 0;
}

static int __memseg_alloc(memseg_t **_memseg, size_t size)
{
        void *addr, *ptr;
        memseg_t *memseg;
        int ret;

        memseg = ltg_malloc1(sizeof(memseg_t));
        if (memseg == NULL) {
                ret = ENOMEM;
                GOTO(err_ret, ret);
        }

        addr = ltg_malloc1(size + HUGEPAGE_SIZE * 512); /*多分配1个G,用于1gb对齐*/
        if (addr == NULL) {
                ret = ENOMEM;
                GOTO(err_ret, ret);
        }

        ptr = __memalign(addr);

        memseg->malloc_addr =addr;
        memseg->start_addr = ptr;

        *_memseg = memseg;

        return 0;

err_ret:
        return ret;
}

static int __memseg_init(size_t *memseg_size, int numa)
{
        int i, ret, memseg_count;
        size_t size;
        memseg_t *memseg;
 
        size = __max_malloc_size(*memseg_size);
        if (size == 0) {
                ret = ENOMEM;
                GOTO(err_ret, ret);
        }

        LTG_ASSERT(size % hugepage_size == 0);

        memseg_count = *memseg_size / size;
       
        for(i = 0; i < memseg_count; i++) {

                ret = __memseg_alloc(&memseg, size);
                if(ret)
                        GOTO(err_ret, ret);

                memseg->size = size;
                memseg->numa = numa;
                LTG_ASSERT((memseg->size % RDMA_MEMREG_SIZE) == 0);

                ltg_spin_init(&memseg->lock);
                INIT_LIST_HEAD(&memseg->slice_list);
                __init_memseg_slice(memseg);
                list_add_tail(&memseg->memseg_list, &memseg_head[numa]);

                ret = __map_hugepages(memseg->start_addr, size / hugepage_size);
                if (ret)
                        GOTO(err_ret, ret);
        }

        *memseg_size =  size;

        return 0;
err_ret:
        return ret;
}

int memseg_init(int daemon, int nr_2mb_hugepage)
{ 
        size_t memseg_size, total_size;
        int i, ret;

        ret = numa_available();
        if (ret < 0) 
                LTG_ASSERT(0);

        numa_count = numa_num_configured_nodes();
        
        if (daemon == 0 || nr_2mb_hugepage / numa_count < (1024 * 4)) {
                ltgconf_global.nr_hugepage = 0;
                use_huge = 0;
                return 0;
        }
 
        hugepage_size = gethugepagesize();

        LTG_ASSERT(numa_count < MAX_NUMA);
        LTG_ASSERT(nr_2mb_hugepage > 0);
        
        total_size = (size_t)nr_2mb_hugepage * HUGEPAGE_SIZE;

        total_size /=  (8 * numa_count);

        if ((total_size << 34) & (((uint64_t)1 << 30) - 1) << 34) {
                DWARN("inval mem size %lu %d\n", total_size, numa_count);
                ret =  EINVAL;
                GOTO(err_ret, ret);
        }

        memseg_size = ((size_t)nr_2mb_hugepage * HUGEPAGE_SIZE) / numa_count;
        
        for (i = 0 ; i < numa_count; i++) {
                INIT_LIST_HEAD(&memseg_head[i]);
                ret = __memseg_init(&memseg_size, i); 
                if (ret)
                        GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}


inline static void __slice_alloc_hpage(memslice_t *slice, uint32_t size)
{
        int i;
        hpage_t *hpage;
        
        slice->hp_count = MEMSLICE_SIZE / size;
        slice->hp_size = size;
        for (i = 0; i < slice->hp_count; i++) {
                hpage = &slice->hpage[i];
                hpage->vaddr = slice->addr + i * size;
                hpage->size = size;
                hpage->slice = slice;
        }

}

inline static memslice_t* __get_free_slice(int _numa, uint32_t size)
{
        struct list_head *pos, *n;
        memseg_t  *memseg;
        memslice_t *slice = NULL;
        int ret, numa;

        if (unlikely(_numa < 0)) {
                next_numa %= numa_count;
                numa = next_numa;
                next_numa += 1;
        } else {
                next_numa %= numa_count;
                numa = _numa;
        }
        list_for_each_safe(pos, n, &memseg_head[numa]) {
                memseg = (memseg_t *)pos;
                ret = ltg_spin_lock(&memseg->lock);
                if (ret)
                        continue;

                if (list_empty(&memseg->slice_list)) {
                        ltg_spin_unlock(&memseg->lock);
                        continue;
                }

                slice = (memslice_t *)memseg->slice_list.next;
                list_del(&slice->list);
                ltg_spin_unlock(&memseg->lock);
                
                if (slice->map == 0) {
                        __slice_bind(slice, numa);
                }


                __slice_alloc_hpage(slice, size);
                break;
        }

        return slice;
}

static void  __get_free_hpage(int numa, struct list_head *head, uint32_t size)
{
        int i;
        memslice_t *slice;
        hpage_t *hpage;

        slice = __get_free_slice(numa, size);
        if (slice == NULL) {
                LTG_ASSERT(0);
        }

        for (i = 0; i < slice->hp_count; i++) {
                hpage = &slice->hpage[i];
                hpage->size = size;
                hpage->vaddr = slice->addr + i * size;
                list_add_tail(&hpage->list, head);
        }
}

int private_hugepage_init(int numa, void **ptr)
{
        int i;
        uint32_t size;

        if (use_huge == 0) {
                *ptr = NULL;
                return 0;
        }

        for(i = 0; i < MAX_SIZE; i++) {
                INIT_LIST_HEAD(&__private_hp__.list[i]);
                if (i == 0) {
                        size = (i + 1) * HUGEPAGE_SIZE;
                        __get_free_hpage(numa, &__private_hp__.list[i], size);
                }
        }

        __private_hp__.numa = numa;

        *ptr = (void *)&__private_hp__;

        return 0;
}

int hugepage_init(void **ptr)
{
        int i;
        uint32_t size;

        if (use_huge == 0) {
                *ptr = NULL;
                return 0;
        }

        for(i = 0; i < MAX_SIZE; i++) {
                INIT_LIST_HEAD(&__hp__.list[i]);
                if (i == 0) {
                        size = (i + 1) * HUGEPAGE_SIZE;
                        __get_free_hpage(0, &__hp__.list[i], size);
                }
        }

        ltg_spin_init(&__hp__.hp_lock);
        __hp__.numa = -1;

        *ptr = (void *)&__hp__;

        return 0;
}

hpage_t* hpage_get(void *hp_list, int offset)
{
        hpage_head_t *head = hp_list;
        hpage_t *hpage;
        uint32_t size;

        if (unlikely(head->numa < 0)) {
                LTG_ASSERT(head == &__hp__);
                ltg_spin_lock(&head->hp_lock);
        } else {
                LTG_ASSERT(head == &__private_hp__);
        }
         
        if (unlikely(list_empty(&head->list[offset]))) {
                size = (offset + 1) * HUGEPAGE_SIZE;
                __get_free_hpage(head->numa, &head->list[offset], size);
        }

        hpage = (hpage_t *)head->list[offset].next;
        list_del(&hpage->list);
        hpage->type = offset;
        if (unlikely(head->numa < 0))
                ltg_spin_unlock(&head->hp_lock);

/*        slice = hpage->slice;
        slice->hp_count--;*/

        return hpage;
}

void hpage_free(void *hp_list, int offset, hpage_t *hpage)
{
        hpage_head_t *head = hp_list;
        
        if (unlikely(head->numa < 0)) {
                LTG_ASSERT(head == &__hp__);
                ltg_spin_lock(&head->hp_lock);
        } else {
                LTG_ASSERT(head == &__private_hp__);
        }

        list_add_tail(&hpage->list, &head->list[offset]);
        
        if (unlikely(head->numa < 0))
                ltg_spin_unlock(&head->hp_lock);

}

int hugepage_getfree(void **addr, uint32_t *size, const char *caller)
{
        (void)caller;
        void *head;
        hpage_t *hpage;

        if (unlikely(use_huge == 0)) {
                *addr = ltg_malloc1(*size);
                //ret = posix_memalign(addr, 4096, *size);
                if (*addr == NULL) 
                        LTG_ASSERT(0);

                return 0;
        }

        core_t *core = core_self();

        if (core == NULL)  {
                head = &__hp__;
        } else
                head = &__private_hp__;

        hpage = hpage_get(head,  (*size - 1)/ HUGEPAGE_SIZE);

        *addr = hpage->vaddr;
        return 0;
}

int memseg_walk(int (*func)(void *addr, void *arg, size_t size), void *arg)
{
        struct list_head *pos, *memseg_list;
        memseg_t *memseg;
        int i, ret;

        if (use_huge == 0)
                return 0;

        for (i = 0; i < numa_count; i++) {
                memseg_list = &memseg_head[i];

                list_for_each(pos, memseg_list) {
                        memseg = (memseg_t *)pos;
                        ret = func(memseg->start_addr, arg, memseg->size);
                        if (ret)
                                LTG_ASSERT(0);

                }
        }

        return 0;
}
