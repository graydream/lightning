#ifndef __MALLOC_H__
#define __MALLOC_H__

extern int ltg_malloc(void **ptr, size_t size);
extern int ltg_malign(void **_ptr, size_t align, size_t size);
extern int ltg_realloc(void **_ptr, size_t size, size_t newsize);
extern int ltg_free(void **ptr);

int huge_mem_alloc1(void **_ptr, size_t size);
void huge_mem_free1(void **ptr);
int huge_mem_realloc1(void **_ptr, size_t size, size_t newsize);
void *huge_mem_alloc(size_t size);
void huge_mem_free(void *ptr);

#endif
