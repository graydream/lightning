#include <numaif.h>
#include <sys/mman.h>

#define DBG_SUBSYS S_LTG_MEM

#include "ltg_utils.h"
#include "core/core.h"

typedef struct {
        struct list_head list;
        uint64_t phyaddr;
        void *vaddr;
} hugepage_t;

typedef struct {
        ltg_spinlock_t    lock;

        void             *malloc_addr;
        void             *start_addr;
        void             *private_hp_head[CORE_MAX]; /*polling core hugepage head*/

        int              hash;
        int              hugepage_count;

        struct list_head hugepage_free_list;
        uint32_t         hugepage_free_count;

        hugepage_t       hgpages[0];
} hugepage_head_t;

static hugepage_head_t *__hugepage__;
static __thread hugepage_head_t *__private_huge__ = NULL;
static int __use_huge__ = 0;
static int PRIVATE_HP_COUNT = 0;

static uint64_t __virt2phy(const void *virtaddr)
{
        int fd;
        uint64_t page, physaddr;
        unsigned long virt_pfn;
        int page_size;
        off_t offset;

        /* standard page size */
        page_size = 4096;

        fd = open("/proc/self/pagemap", O_RDONLY);
        if (fd < 0) {
                exit(0);
        }

        virt_pfn = (unsigned long)virtaddr / page_size;
        offset = sizeof(uint64_t) * virt_pfn;
        if (lseek(fd, offset, SEEK_SET) == (off_t) -1) {
                close(fd);
                exit(0);
        }

        if (read(fd, &page, sizeof(uint64_t)) < 0) {
                close(fd);
                exit(0);
        }

        physaddr = ((page & 0x7fffffffffffffULL) * page_size)
                + ((unsigned long)virtaddr % page_size);

        close(fd);

        return physaddr;
}

static void __hugepage_head_init(hugepage_head_t *head, void *mem, void *addr,
                                 int hp_count, int sockid)
{
        long unsigned int sock;

        if (sockid >= 0){
                sock = 1 << sockid;
                mbind(addr, HUGEPAGE_SIZE, MPOL_PREFERRED, &sock, 3, 0);
        }

        memset(addr, 0x00, HUGEPAGE_SIZE);

        head->start_addr = addr + HUGEPAGE_SIZE;
        head->hugepage_count = hp_count;
        INIT_LIST_HEAD(&head->hugepage_free_list);

        head->malloc_addr = mem;
        head->hash = -1;
        
        int ret = ltg_spin_init(&head->lock);
        LTG_ASSERT(ret == 0);
}

static int __map_hugepages(void *addr, int map, int hp_count)
{
        int i, j, ret;
        char *map_ret;
        void *map_addr = addr;
        void *unmap_addr = addr;

        if (!map)
                return 0;

        for (i = 0; i < hp_count; i++) {
                map_ret = mmap(map_addr, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE |MAP_FIXED|MAP_ANONYMOUS|0x40000 , -1, 0);
                if (map_ret == MAP_FAILED) {
                        DINFO("mmap erro  %p %d %s\n", map_addr, hp_count, strerror(errno))
                                ret = ENOMEM;
                        GOTO(err_ret, ret);
                }

                map_addr += HUGEPAGE_SIZE;
        }

        return 0;

err_ret:
        for (j = 0; j < i; j++) {
                munmap(unmap_addr, HUGEPAGE_SIZE);
                unmap_addr += HUGEPAGE_SIZE;
        }
        return ret;
}

static void __hugepage_init(hugepage_t *hpage, void *addr, int sockid, int idx,
                            int map)
{
        unsigned long int  sock;
        uint64_t phyaddr = 0;

        (void) idx;
        
        if (sockid >= 0){
                sock = 1 << sockid;
                mbind(addr, HUGEPAGE_SIZE, MPOL_PREFERRED, &sock, 3, 0);
        }

        if (map) {
                memset(addr, 0x00, HUGEPAGE_SIZE);
                phyaddr = __virt2phy(addr);
        }

        hpage->vaddr = addr;
        hpage->phyaddr = phyaddr;

        return ;
}

void *hugepage_private_init(int hash, int sockid)
{
        hugepage_head_t *head;
        hugepage_t      *hpage;
        int i ;

        if (__use_huge__ == 0) {
                return NULL;
        }
        
        void *addr = __hugepage__->private_hp_head[hash];
        
        LTG_ASSERT(addr);
        
        DINFO("hash %d beigin init private hugepage%p\n", hash, addr);
        head = (hugepage_head_t *)addr;

        __hugepage_head_init(head, NULL, addr, PRIVATE_HP_COUNT, sockid);

        for (i = 0; i < PRIVATE_HP_COUNT; i++) {
                addr += HUGEPAGE_SIZE;
                hpage = &head->hgpages[i];
                __hugepage_init(hpage, addr, sockid, i, __use_huge__);
                list_add_tail(&hpage->list, &head->hugepage_free_list);
                //hpage->head = head;
        }

        head->hash = hash;

        DINFO("init private hugepage finish \n");
        __private_huge__ = head;

        return head;
}

static void __hugepage_init_public(hugepage_head_t *head, void *public, int daemon, int use_huge)
{
        hugepage_t *hpage;
        void *pos = public;

        for (int i = 0; i < PUBLIC_HP_COUNT; i++) {
                hpage = &head->hgpages[i];
                __hugepage_init(hpage, pos,  -1, i, daemon & use_huge);
                list_add_tail(&hpage->list, &head->hugepage_free_list);
                pos += HUGEPAGE_SIZE;
        }

        __hugepage__ = head;
        __use_huge__ = use_huge;
}

static void __hugepage_init_private(hugepage_head_t *head, void *private)
{
        void *pos = private;
        
        for (int i = 0; i < CORE_MAX; i++) {
                if (!core_used(i)) {
                        head->private_hp_head[i] = NULL;
                        continue;
                }
                
                head->private_hp_head[i] = pos;
                pos += (PRIVATE_HP_COUNT  + 1) * HUGEPAGE_SIZE;

                DINFO("core[%d] head 0x%p\n", i, pos);
        
        }
}

int hugepage_init(int daemon, uint64_t coremask, int nr_hugepage)
{ 
        int ret, hp_count, poll_num = 0;
        size_t mem_size;
        void *mem, *addr, *private, *public;
        size_t align;
        hugepage_head_t *head;

        if (nr_hugepage == 0) {
                __use_huge__ = 0;
                return 0;
        }
        
        static_assert(sizeof(*head) <= HUGEPAGE_SIZE, "hugepage");
        PRIVATE_HP_COUNT = nr_hugepage;
        
        for (int i = 0; i < CORE_MAX; i++) {
                if (core_usedby(coremask, i))
                        poll_num++;
        }
        
        if (daemon) {
                mem_size = ((LLU)PRIVATE_HP_COUNT  + 1) * HUGEPAGE_SIZE * poll_num
                        + (PUBLIC_HP_COUNT + 2) * HUGEPAGE_SIZE;
                hp_count = (PRIVATE_HP_COUNT  + 1) * poll_num 
                        + PUBLIC_HP_COUNT + 1;
        } else  {
                mem_size = (PUBLIC_HP_COUNT + 2) * HUGEPAGE_SIZE;
                hp_count = PUBLIC_HP_COUNT + 1;
        }

        mem = malloc(mem_size);
        if (mem == NULL) {
                DINFO("malloc %d error\n", mem_size);
                ret = ENOMEM;
                GOTO(err_ret, ret);
        }

        align = (size_t)mem & (HUGEPAGE_SIZE - 1);
        if (align){
                addr = mem + HUGEPAGE_SIZE - align;
        } else
                addr = mem;

        ret = __map_hugepages(addr, daemon & nr_hugepage, hp_count);
        if (ret)
                GOTO(err_ret, ret);

        head = addr;
        public = addr + HUGEPAGE_SIZE;
        private = public + (PUBLIC_HP_COUNT) * HUGEPAGE_SIZE;

        __hugepage_head_init(head, mem, addr, hp_count - 1, -1);

        __hugepage_init_public(head, public, daemon, nr_hugepage);

        if (daemon) {
                __hugepage_init_private(head, private);
        }

        return 0;
err_ret:
        return ret;
}

void private_hugepage_free(void *addr)
{   
        (void)addr;

        return ;
}

#if ENABLE_HUGEPAGE
static hugepage_t *__get_free_hugepage(hugepage_head_t *head)
{
        int ret;
        hugepage_t *hpage;
        
        if (unlikely(head == __hugepage__)) {
                ret = ltg_spin_lock(&head->lock);
                LTG_ASSERT(ret == 0);
                
                hpage = list_entry(head->hugepage_free_list.next, hugepage_t, list);        
                list_del(&hpage->list);

                ltg_spin_unlock(&head->lock);
        } else {
                hpage = list_entry(head->hugepage_free_list.next, hugepage_t, list);        
                list_del(&hpage->list);
        }

        return hpage;       
}

static int __hugepage_non_getfree(void **_addr, uint64_t *phyaddr)
{
        int ret;
        void *addr;

        DINFO("fake hugepage_getfree\n");
        
        ret = ltg_malign((void **)&addr, PAGE_SIZE, HUGEPAGE_SIZE);
        if (ret)
                UNIMPLEMENTED(__DUMP__);

        *_addr = addr;
        if (phyaddr) {
                *phyaddr = __virt2phy(addr);
        }
        
        return 0;
}

int hugepage_getfree(void **_addr, uint64_t *phyaddr)
{
        int ret;
        hugepage_t *hpage;
        hugepage_head_t *head = __private_huge__ ? __private_huge__ : __hugepage__;

        if (unlikely(__use_huge__ == 0)) {
                return __hugepage_non_getfree(_addr, phyaddr);
        }
        
        if (unlikely(list_empty(&head->hugepage_free_list))) {
                DERROR("hugepage full\n");
                EXIT(EAGAIN);
                ret = ENOMEM;
                GOTO(err_ret, ret);
        }

        core_t *core = core_self();
        if (likely(core)) {
                DINFO("%s[%d] hugepage getfree\n", core->name, core->hash);
        } else {
                DINFO("none[-1] hugepage getfree\n");
        }
        
        hpage = __get_free_hugepage(head);

        if (phyaddr)
                *phyaddr = hpage->phyaddr;

        *_addr = hpage->vaddr;
        
        return 0;
err_ret:
        return ret;
}

#else

int hugepage_getfree(void **_addr, uint64_t *phyaddr)
{
        int ret;
        void *addr;

        DINFO("fake hugepage_getfree\n");
        
        ret = ltg_malign((void **)&addr, PAGE_SIZE, HUGEPAGE_SIZE);
        if (ret)
                UNIMPLEMENTED(__DUMP__);

        *_addr = addr;
        if (phyaddr) {
                *phyaddr = (uint64_t)addr;
        }
        
        return 0;
}
#endif

void get_global_private_mem(void **private_mem, uint64_t *private_mem_size)
{
        *private_mem = __hugepage__->start_addr;
        *private_mem_size = ((uint64_t)__hugepage__->hugepage_count) * HUGEPAGE_SIZE;
}
