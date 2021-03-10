#ifndef __LTGBUF_H__
#define __LTGBUF_H__

#include <sys/uio.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <errno.h>
#include <stdbool.h>
#include <infiniband/verbs.h>

#include "utils/ltg_list.h"
#include "mem/mem_ring.h"

typedef struct seg_t seg_t;
typedef struct ltgbuf_t ltgbuf_t;
typedef struct ltgbuf_t buf_t;

typedef struct{         
        seg_t *(*seg_share)(ltgbuf_t *buf, seg_t *seg);
        seg_t *(*seg_trans)(ltgbuf_t *buf, seg_t *seg);
        void (*seg_free)(seg_t *seg); 
} seg_ops_t;

#pragma pack(4)

typedef struct {
        void *head;
        void *pool;
} mem_huge_t;

typedef struct {
        void *base;
} mem_sys_t;

typedef struct {
        void *base;
} mem_solid_t;


typedef struct {
        void *arg;
        int (*cb)(void *arg);
} mem_ext_t;

struct seg_t {
        struct list_head hook;
        uint32_t len;
        uint8_t local;
        uint8_t shared;
        uint16_t __pad__;
        seg_ops_t *sop;

        struct {
                void *ptr;
                uint64_t phyaddr;
        } handler;
        
        union {
                mem_huge_t huge;
                mem_ext_t ext;
                mem_sys_t sys;
                mem_solid_t solid;
        };
} ;

#define SEG_KEEP 5

struct ltgbuf_t {
        uint32_t len;
        uint32_t used;
        struct list_head list;
        struct list_head keep;
        seg_t array[SEG_KEEP];
};

#pragma pack()

typedef struct {
        uint32_t size;
        uint64_t offset;
        void     *addr;
        uint64_t phyaddr;
        ltgbuf_t buf;
} nvmeio_t;

#define USEC()                                                \
        do {                                                  \
                struct timeval __tv__;                        \
                gettimeofday(&__tv__, NULL);                  \
                DWARN("USEC %llu\n", (LLU)__tv__.tv_usec);    \
        } while (0);

typedef int (*buf_itor_func)(void *, void *addr, uint64_t phyaddr, size_t len);

int ltgbuf_init(ltgbuf_t *pack, uint32_t size);
void ltgbuf_free(ltgbuf_t *pack);

int ltgbuf_appendmem(ltgbuf_t *buf, const void *src, uint32_t len);

int ltgbuf_appendzero(ltgbuf_t *buf, int size);

int ltgbuf_copy(ltgbuf_t *buf, const char *srcmem, int size);
int ltgbuf_copy1(ltgbuf_t *pack, const void *buf, uint32_t offset, uint32_t len);
void ltgbuf_copy2(ltgbuf_t *buf, const char *srcmem, int size);
void ltgbuf_copy3(ltgbuf_t *buf, const char *srcmem, int size);
int ltgbuf_droptail(ltgbuf_t *buf, uint32_t len);
int ltgbuf_segcount(const ltgbuf_t *buf);

int ltgbuf_aligned(const ltgbuf_t *buf, int align);
int ltgbuf_popmsg(ltgbuf_t *pack, void *buf, uint32_t len);

int ltgbuf_pop(ltgbuf_t *buf, ltgbuf_t *newbuf, uint32_t len);
int ltgbuf_pop1(ltgbuf_t *buf, ltgbuf_t *newbuf, uint32_t len, int deep);

void ltgbuf_merge(ltgbuf_t *dist, ltgbuf_t *src);
void ltgbuf_reference(ltgbuf_t *dist, const ltgbuf_t *src);
void ltgbuf_clone1(ltgbuf_t *newbuf, const ltgbuf_t *buf, int init);
void ltgbuf_clone(ltgbuf_t *dist, const ltgbuf_t *src);
void ltgbuf_clone_glob(ltgbuf_t *newbuf, const ltgbuf_t *buf);

uint32_t ltgbuf_crc_stream(uint32_t *crcode, const ltgbuf_t *buf, uint32_t offset, uint32_t size);
uint32_t ltgbuf_crc(const ltgbuf_t *buf, uint32_t _off, uint32_t size);
extern int ltgbuf_appendzero(ltgbuf_t *buf, int size);
extern int ltgbuf_writefile(const ltgbuf_t *buf, int fd, uint64_t offset);

void *ltgbuf_head(const ltgbuf_t *buf);
void *ltgbuf_head1(const ltgbuf_t *buf, uint32_t size);
int ltgbuf_get(const ltgbuf_t *pack, void *buf, uint32_t len);
int ltgbuf_get1(const ltgbuf_t *pack, void *buf, uint32_t offset, uint32_t len);

uint32_t ltgbuf_crc(const ltgbuf_t *buf, uint32_t _off, uint32_t size);
int ltgbuf_writefile(const ltgbuf_t *buf, int fd, uint64_t offset);

int ltgbuf_trans(struct iovec *_iov, int *iov_count, const ltgbuf_t *buf);
int ltgbuf_trans1(struct iovec *_iov, int *iov_count, uint32_t offset, const ltgbuf_t *buf);
void ltgbuf_trans2(struct iovec *_iov, int *iov_count, uint32_t offset,
                    uint32_t size, const ltgbuf_t *buf);
int ltgbuf_trans3(struct iovec *_iov, int *iov_count, ltgbuf_t *buf, size_t iov_max);


int ltgbuf_compress(ltgbuf_t *buf);
int ltgbuf_compress2(ltgbuf_t *buf, uint32_t max_seg_len);

int ltgbuf_dump(const ltgbuf_t *buf, uint32_t len, const char *s);
int ltgbuf_itor(const ltgbuf_t *buf, uint32_t size, off_t offset,
                 buf_itor_func func, void *ctx);
void ltgbuf_bezero(ltgbuf_t *buf);
void ltgbuf_check(const ltgbuf_t *buf);

int ltgbuf_solid_init(ltgbuf_t *buf, int size);

int ltgbuf_trans_sge(struct ibv_sge *sge, ltgbuf_t *src_buf, ltgbuf_t *dst_buf,  struct ibv_mr* (*get_mr)(uint64_t *mr_map, const void *addr), void *mr_map);
void ltgbuf_trans_addr(void **addr, const ltgbuf_t *buf);
int ltgbuf_initwith(ltgbuf_t *buf, void *data, int size, void *arg, int (*cb)(void *arg));
int ltgbuf_initwith2(ltgbuf_t *buf, struct iovec *iov, int count, void *arg, int (*cb)(void *arg));
int ltgbuf_rdma_popmsg(ltgbuf_t *buf, void *dist, uint32_t len);
int ltgbuf_nvme_init2(nvmeio_t *io, uint32_t size, uint64_t offset, ltgbuf_t *buf);

void seg_init();
seg_t *seg_solid_create(ltgbuf_t *buf, uint32_t size);
seg_t *seg_huge_create(ltgbuf_t *buf, uint32_t *size);
seg_t *seg_sys_create(ltgbuf_t *buf, uint32_t size);
seg_t *seg_ext_create(ltgbuf_t *buf, void *data, uint32_t size,
                      void *arg, int (*cb)(void *arg));
seg_t *seg_trans(ltgbuf_t *buf, seg_t *seg);
void seg_check(seg_t *seg);

void seg_add_tail(ltgbuf_t *buf, seg_t *seg);

#endif
