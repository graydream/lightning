#ifndef __SLAB_H__
#define __SLAB_H__


#define SLAB_SEG_MAX (1024 * 10)

typedef struct {
        char name[MAX_NAME_LEN];
        size_t split;
        int private;
        int count;
        pid_t tid;
        struct list_head list;
        struct list_head used;
        void *array[SLAB_SEG_MAX];
} slab_bucket_t;

typedef struct {
        struct list_head hook;
        time_t time;
        uint32_t magic;
        uint32_t coreid;
        slab_bucket_t *slab_bucket;
        void *seghead;
        char ptr[0];
} slab_md_t;

typedef struct {
        ltg_spinlock_t spin;
        uint32_t magic;
        int count;
        pid_t tid;
        uint32_t coreid;
        slab_bucket_t slab_bucket[0];
} slab_array_t;

typedef struct {
        size_t max;
        slab_array_t *public;
        slab_array_t *private;
} slab_t;


int slab_init(const char *name, slab_t **_slab, slab_array_t **_public, int min,
              int shift, uint32_t magic);
int slab_private_init(const char *name, slab_t **_slab, slab_array_t *public, int min,
                      int shift, uint32_t magic);
void *slab_alloc(slab_t *slab, size_t size);
void slab_free(slab_t *slab, void *ptr);
void slab_scan(void *core, slab_array_t *array);

int slab_static_init();
int slab_static_private_init();
void *slab_static_alloc(size_t size);
void slab_static_free(void *ptr);
void *slab_static_alloc_glob(size_t size);
int slab_static_alloc1(void **_ptr, size_t size);
void slab_static_free1(void **ptr);

int slab_stream_init();
int slab_stream_private_init();
void *slab_stream_alloc(size_t size);
void slab_stream_free(void *ptr);
void *slab_stream_alloc_glob(size_t size);
int slab_stream_alloc1(void **_ptr, size_t size);
void slab_stream_free1(void **ptr);

#endif
