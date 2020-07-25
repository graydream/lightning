#include "ltg_utils.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>

#include "mem/buddy.h"

#define LEFT_LEAF(index) ((index) * 2 + 1)
#define RIGHT_LEAF(index) ((index) * 2 + 2)
#define PARENT(index) ( ((index) + 1) / 2 - 1)

#define IS_POWER_OF_2(x) (!((x)&((x)-1)))
#define MAX(a, b) ((a) > (b) ? (a) : (b))


//this will used for future, flexiable swith between ymalloc.
struct mem_alloc mem_buddy;

int buddy_init(void *buddy_addr, uint32_t page_num)
{
        struct buddy *buddy = (struct buddy *)buddy_addr;
        size_t i;
        size_t node_size;

        buddy->nr_total = page_num;
        buddy->nr_alloc = 0;

        node_size = (size_t)page_num * 2;

        for (i = 0; i < page_num * 2 - 1; i++) {
                if (IS_POWER_OF_2(i + 1))
                        node_size /= 2;

                buddy->buddy_trees[i] = node_size;
        }

        mem_buddy.max_alloc_size = MAX_ALLOC_SIZE;

        BUDDY_DUMP_L(DINFO, buddy, "\n");
        return 0;
}

int __buddy_alloc(void *buddy_addr, uint32_t size)
{
        struct buddy *buddy = (struct buddy *)buddy_addr;
        size_t index = 0;
        size_t node_size;
        size_t offset = 0;

        LTG_ASSERT(size > 0);

        if (!IS_POWER_OF_2(size))
                LTG_ASSERT(0);

        if (buddy->buddy_trees[index] < size) {
                //UNIMPLEMENTED(__DUMP__);
                BUDDY_DUMP_L(DWARN, buddy, "alloc %u\n", size);
                return -1;
        }

        for (node_size = buddy->nr_total; node_size != size; node_size /= 2) {
                if (buddy->buddy_trees[LEFT_LEAF(index)] >= size)
                        index = LEFT_LEAF(index);
                else
                        index = RIGHT_LEAF(index);
        }

        buddy->buddy_trees[index] = 0;
        offset = (index + 1) * node_size - buddy->nr_total;

        while (index) {
                index = PARENT(index);
                buddy->buddy_trees[index] = MAX(buddy->buddy_trees[LEFT_LEAF(index)],
                                                buddy->buddy_trees[RIGHT_LEAF(index)]);
        }

        buddy->nr_alloc += size;
        BUDDY_DUMP_L(DINFO, buddy, "alloc %u\n", size);

        return offset;
}

int buddy_alloc(void *buddy_addr, void **_addr, uint32_t *size)
{
        uint32_t  req_size = _min(*size, mem_buddy.max_alloc_size);
        int index;
        void *start_addr = (void *)((uint64_t)buddy_addr & (~(((uint64_t)1 << 21) -1))) + HUGEPAGE_SIZE;

        index = __buddy_alloc(buddy_addr, req_size >> 21);
        if (unlikely(index < 0)) {
                    DERROR("hugepage full, index %d size %u\n", index, *size);
                    EXIT(EAGAIN);
        }

        *size = req_size;

        *_addr = start_addr + index * HUGEPAGE_SIZE;

        DINFO("buddy start addr %p alloc addr %p size %u\n",
              start_addr, *_addr, *size);

        return 0;
}

int __buddy_free(void *buddy_addr, uint32_t offset)
{
        struct buddy *buddy = (struct buddy *)buddy_addr;
        size_t node_size, index = 0;
        size_t left_longest, right_longest;

        LTG_ASSERT(buddy && offset < buddy->nr_total);
        node_size = 1;
        index = offset + buddy->nr_total - 1;

        for (; buddy->buddy_trees[index]; index = PARENT(index)) {
                node_size *= 2;
                if (index == 0)
                        return 0;
        }

        buddy->buddy_trees[index] = node_size;

        while (index) {
                index = PARENT(index);
                node_size *= 2;

                left_longest = buddy->buddy_trees[LEFT_LEAF(index)];
                right_longest = buddy->buddy_trees[RIGHT_LEAF(index)];

                if (left_longest + right_longest == node_size)
                        buddy->buddy_trees[index] = node_size;
                else
                        buddy->buddy_trees[index] = MAX(left_longest, right_longest);
        }

        return node_size;
}

int buddy_free(void *buddy_addr, void *free_addr, uint32_t size)
{
        (void)buddy_addr;
        (void)free_addr;
        (void)size;
        return 0;
}

void buddy_memalloc_reg()
{
        suzaku_mem_alloc_register(&mem_buddy);
}

struct mem_alloc mem_buddy = {
        .type = 0,      //todo, buddy type.
        .init = buddy_init,
        .alloc = buddy_alloc,
        .free = buddy_free
};

#if 0
SUZAKU_MEM_ALLOC_REGISTER(buddy, &mem_buddy);
#endif
