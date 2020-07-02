#ifndef __BUDDY_H__
#define __BUDDY_H__

typedef struct buddy {
        size_t mpage_count;
        size_t buddy_trees[0];
} buddy_t;

#define BUDDY_SIZE(count) (sizeof(size_t) + count * sizeof(size_t))

#endif
