#ifndef __MALLOC_H__
#define __MALLOC_H__

void *ltg_malloc1(size_t size);
int ltg_malloc(void **ptr, size_t size);
int ltg_malign(void **_ptr, size_t align, size_t size);
int ltg_realloc(void **_ptr, size_t size, size_t newsize);
int ltg_free(void **ptr);

int huge_mem_alloc1(void **_ptr, size_t size);
void huge_mem_free1(void **ptr);
int huge_mem_realloc1(void **_ptr, size_t size, size_t newsize);
void *huge_mem_alloc(size_t size);
void huge_mem_free(void *ptr);

#endif
