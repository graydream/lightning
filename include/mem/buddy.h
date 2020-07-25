#ifndef __BUDDY_H__
#define __BUDDY_H__

typedef struct buddy {
        uint32_t nr_total;
        uint32_t nr_alloc;
        size_t buddy_trees[0];
} buddy_t;

#define BUDDY_SIZE(count) (sizeof(size_t) + count * sizeof(size_t))

#define BUDDY_DUMP_L(LEVEL, buddy, format, a...) do { \
    LEVEL("buddy %p hugepage %u/%u "format, \
          (buddy),  \
          (buddy)->nr_total,  \
          (buddy)->nr_alloc,  \
          ##a \
          ); \
} while(0)

#define BUDDY_DUMP(buddy, format, a...) BUDDY_DUMP_L(DBUG, buddy, format, ##a)

#endif
