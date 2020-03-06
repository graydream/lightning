#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>

#define DBG_SUBSYS S_LTG_MEM

#include "ltg_utils.h"
#include "core/core.h"

#if 0
#define BUFFER_DEBUG
#endif

#ifdef BUFFER_DEBUG
#define BUFFER_CHECK(buf)                                               \
        do {                                                            \
                uint32_t __len = 0;                                     \
                seg_t *__seg;                                           \
                struct list_head *pos;                                  \
                list_for_each(pos, &buf->list) {                        \
                        __seg = (seg_t *)pos;                           \
                        seg_check(__seg);                                 \
                        __len += __seg->len;                            \
                }                                                       \
                if (__len != (buf)->len) {                              \
                        DERROR("__len %u (buf)->len %u\n", __len, (buf)->len); \
                        LTG_ASSERT(0);                                     \
                }                                                       \
        } while (0)

#else
#define BUFFER_CHECK(buf)
#endif

#undef BLOCK_SIZE
#define BLOCK_SIZE 512

inline static int __coreid()
{
        core_t *core = core_self();

        if (unlikely(core == NULL))
                return -1;
        else
                return core->hash;
}

int ltgbuf_rdma_popmsg(ltgbuf_t *buf, void *dist, uint32_t len)
{
        struct list_head *pos;
        seg_t *seg;

        pos = buf->list.next;

        seg = (seg_t *)pos;
          
        memcpy(dist, seg->handler.ptr, len);
        //*dist = seg->handler.ptr;
        seg->handler.ptr += len;
        seg->handler.phyaddr += len;
        seg->len -= len;
       
        if (seg->len == 0) {
                list_del(pos);
                seg->sop.seg_free(seg);
        }

        buf->len -= len;

        return 0;
}

int ltgbuf_get(const ltgbuf_t *buf, void *dist, uint32_t len)
{
        struct list_head *pos;
        seg_t *seg;
        uint32_t left, cp;

        LTG_ASSERT(buf->len >= len);

        BUFFER_CHECK(buf);

        left = len;

        list_for_each(pos, &buf->list) {
                seg = (seg_t *)pos;
                cp = left < seg->len ? left : seg->len;
                memcpy(dist + (len - left), seg->handler.ptr, cp);
                
                left -= cp;
                if (left == 0)
                        break;
        }

        BUFFER_CHECK(buf);

        LTG_ASSERT(left == 0);

        return 0;
}

int ltgbuf_get1(const ltgbuf_t *buf, void *dist, uint32_t offset, uint32_t len)
{
        uint32_t buf_off = 0, seg_off, dist_off = 0;
        struct list_head *pos;
        seg_t *seg;
        uint32_t left, cp;

        LTG_ASSERT(buf->len >= offset + len);

        BUFFER_CHECK(buf);

        left = len;

        list_for_each(pos, &buf->list) {
                seg = (seg_t *)pos;

                if (buf_off + seg->len <= offset) {
                        cp = 0;
                } else if (buf_off < offset && buf_off + seg->len <= offset + len) {
                        seg_off = offset - buf_off;
                        cp = seg->len - seg_off;
                        memcpy(dist + dist_off, seg->handler.ptr + seg_off, cp);
                } else if (buf_off < offset && buf_off + seg->len > offset + len) {
                        seg_off = offset - buf_off;
                        cp = len;
                        memcpy(dist + dist_off, seg->handler.ptr + seg_off, cp);
                } else if (buf_off >= offset && buf_off + seg->len <= offset + len) {
                        cp = seg->len;
                        memcpy(dist + dist_off, seg->handler.ptr, cp);
                } else if (buf_off >= offset && buf_off + seg->len> offset + len) {
                        cp = (offset + len) - buf_off;
                        memcpy(dist + dist_off, seg->handler.ptr, cp);
                } else if (buf_off >= offset + len) {
                        cp = 0;
                } else {
                        LTG_ASSERT(0);
                }

                buf_off += seg->len;
                dist_off += cp;
                left -= cp;

                if (left == 0)
                        break;
        }

        BUFFER_CHECK(buf);

        LTG_ASSERT(left == 0);

        return 0;
}

int ltgbuf_copy1(ltgbuf_t *buf, void *src, uint32_t offset, uint32_t len)
{
        uint32_t buf_off = 0, seg_off, src_off = 0;
        struct list_head *pos;
        seg_t *seg;
        uint32_t left, cp;

        LTG_ASSERT(buf->len >= offset + len);

        BUFFER_CHECK(buf);

        left = len;

        list_for_each(pos, &buf->list) {
                seg = (seg_t *)pos;

                if (buf_off + seg->len <= offset) {
                        cp = 0;
                } else if (buf_off < offset && buf_off + seg->len <= offset + len) {
                        seg_off = offset - buf_off;
                        cp = seg->len - seg_off;
                        memcpy(seg->handler.ptr + seg_off, src + src_off, cp);
                } else if (buf_off < offset && buf_off + seg->len > offset + len) {
                        seg_off = offset - buf_off;
                        cp = len;
                        memcpy(seg->handler.ptr + seg_off, src + src_off, cp);
                } else if (buf_off >= offset && buf_off + seg->len <= offset + len) {
                        cp = seg->len;
                        memcpy(seg->handler.ptr, src + src_off, cp);
                } else if (buf_off >= offset && buf_off + seg->len> offset + len) {
                        cp = (offset + len) - buf_off;
                        memcpy(seg->handler.ptr, src + src_off, cp);
                } else if (buf_off >= offset + len) {
                        cp = 0;
                } else {
                        LTG_ASSERT(0);
                }

                buf_off += seg->len;
                src_off += cp;
                left -= cp;

                if (left == 0)
                        break;
        }

        BUFFER_CHECK(buf);

        LTG_ASSERT(left == 0);

        return 0;
}

int ltgbuf_copy2(void *dst, const ltgbuf_t *buf, uint32_t offset, uint32_t len)
{
        uint32_t buf_off = 0, seg_off, dst_off = 0;
        struct list_head *pos;
        seg_t *seg;
        uint32_t left, cp;

        LTG_ASSERT(buf->len >= offset + len);

        BUFFER_CHECK(buf);

        left = len;

        list_for_each(pos, &buf->list) {
                seg = (seg_t *)pos;

                if (buf_off + seg->len <= offset) {
                        cp = 0;
                } else if (buf_off < offset && buf_off + seg->len <= offset + len) {
                        seg_off = offset - buf_off;
                        cp = seg->len - seg_off;
                        memcpy(dst + dst_off, seg->handler.ptr + seg_off, cp);
                } else if (buf_off < offset && buf_off + seg->len > offset + len) {
                        seg_off = offset - buf_off;
                        cp = len;
                        memcpy(dst + dst_off, seg->handler.ptr + seg_off, cp);
                } else if (buf_off >= offset && buf_off + seg->len <= offset + len) {
                        cp = seg->len;
                        memcpy(dst + dst_off, seg->handler.ptr, cp);
                } else if (buf_off >= offset && buf_off + seg->len> offset + len) {
                        cp = (offset + len) - buf_off;
                        memcpy(dst + dst_off, seg->handler.ptr, cp);
                } else if (buf_off >= offset + len) {
                        cp = 0;
                } else {
                        LTG_ASSERT(0);
                }

                buf_off += seg->len;
                dst_off += cp;
                left -= cp;

                if (left == 0)
                        break;
        }

        BUFFER_CHECK(buf);

        LTG_ASSERT(left == 0);

        return 0;
}

int ltgbuf_popmsg(ltgbuf_t *buf, void *dist, uint32_t len)
{
        struct list_head *pos, *n;
        seg_t *seg;
        uint32_t left, cp;

        //ANALYSIS_BEGIN(0);

        BUFFER_CHECK(buf);

        left = len;

        LTG_ASSERT(dist);
        LTG_ASSERT(buf->len >= len);

        list_for_each_safe(pos, n, &buf->list) {
                seg = (seg_t *)pos;
                cp = left < seg->len ? left : seg->len;

                memcpy(dist + (len - left), seg->handler.ptr, cp);

                if (cp < seg->len) {
                        seg->handler.ptr = seg->handler.ptr + cp;
                        seg->handler.phyaddr += cp;
                        seg->len -= cp;
                } else {
                        list_del(pos);
                        seg->sop.seg_free(seg);
                }

                left -= cp;
                if (left == 0)
                        break;
        }

        LTG_ASSERT(left == 0);

        buf->len -= len;

        BUFFER_CHECK(buf);

        //ANALYSIS_END(0, 100, "popmsg");

        return 0;
}

/**
 * @todo len == 0
 *
 * @param buf
 * @param src
 * @param len len > 0
 * @param tail
 * @return
 */
STATIC int __ltgbuf_appendmem(ltgbuf_t *buf, const void *src, uint32_t len, int glob)
{
        seg_t *seg;

        (void) glob;

        DBUG("append len %u\n", len);

        LTG_ASSERT(len <= BUFFER_SEG_SIZE);

        BUFFER_CHECK(buf);
        if (unlikely(glob)) {
                seg = seg_sys_create(buf, len);
        } else {
                seg = seg_huge_create(buf, len);
        }

        //DINFO("append seg %p, ptr %p\n", seg, seg->handler.ptr);
        memcpy(seg->handler.ptr, src, len);

        seg_add_tail(buf, seg);

        BUFFER_CHECK(buf);

        return 0;
}

int ltgbuf_appendmem(ltgbuf_t *buf, const void *src, uint32_t len) {
        return __ltgbuf_appendmem(buf, src, len, 0);
}

STATIC void __ltgbuf_free1(ltgbuf_t *buf)
{
        int count;
        seg_t *seg;
        struct list_head *pos, *n;

        (void) count;
        count = 0;
        list_for_each_safe(pos, n, &buf->list) {
                seg = (seg_t *)pos;
                //DINFO("free seg %p ptr%p\n", seg, seg->handler.ptr);
                list_del(pos);
                seg->sop.seg_free(seg);
        }

        buf->len = 0;
        buf->used = 0;
}

void ltgbuf_clone_glob(ltgbuf_t *newbuf, const ltgbuf_t *buf)
{
        int ret;
        struct list_head *pos;
        seg_t *seg;

        BUFFER_CHECK(buf);

        ltgbuf_init(newbuf, 0);

        list_for_each(pos, &buf->list) {
                seg = (seg_t *)pos;

                ret = __ltgbuf_appendmem(newbuf, seg->handler.ptr, seg->len, 1);
                if (ret)
                        UNIMPLEMENTED(__DUMP__);
        }

        BUFFER_CHECK(newbuf);
        BUFFER_CHECK(buf);
}

int ltgbuf_trans_sge(struct ibv_sge *sge, ltgbuf_t *src_buf, ltgbuf_t *dst_buf, uint32_t lkey)
{
        struct list_head *pos;
        int num = 0;
        seg_t *seg;

        if (src_buf)
                ltgbuf_merge(dst_buf, src_buf);

        list_for_each(pos, &dst_buf->list) {
                seg = (seg_t *)pos;

                sge[num].addr = (uintptr_t)seg->handler.ptr;
                sge[num].length = seg->len;
                sge[num].lkey = lkey;
                num++;
        }

        return num;
}

inline void IO_FUNC ltgbuf_merge(ltgbuf_t *dist, ltgbuf_t *src)
{
        BUFFER_CHECK(src);
        BUFFER_CHECK(dist);

        struct list_head *pos, *n;
        list_for_each_safe(pos, n, &src->list) {
                list_del(pos);
                seg_t *seg = (seg_t *)pos;
                seg_t *newseg = seg->sop.seg_trans(dist, seg);
                seg_add_tail(dist, newseg);
        }

        src->len = 0;
        src->used = 0;
        BUFFER_CHECK(src);
        BUFFER_CHECK(dist);
}

void IO_FUNC ltgbuf_reference(ltgbuf_t *dist, const ltgbuf_t *src)
{
        seg_t *seg;
        struct list_head *pos;

        BUFFER_CHECK(src);
        BUFFER_CHECK(dist);

        list_for_each(pos, &src->list) {
                seg = ((seg_t *)pos)->sop.seg_share(dist, (void *)pos);

                seg_add_tail(dist, seg);
        }

        BUFFER_CHECK(dist);
}

inline int IO_FUNC ltgbuf_initwith(ltgbuf_t *buf, void *data, int size,
                                    void *arg, int (*cb)(void *arg))
{
        seg_t *seg;

        buf->len = 0;
        buf->used = 0;
        INIT_LIST_HEAD(&buf->list);

        seg = seg_ext_create(buf, data, size, arg, cb);

        seg_add_tail(buf, seg);
        BUFFER_CHECK(buf);

        return 0;
}

inline int ltgbuf_init(ltgbuf_t *buf, int size)
{
        seg_t *seg;

        LTG_ASSERT(size >= 0 && size < (1024 * 1024 * 100));

        buf->len = 0;
        buf->used = 0;
        INIT_LIST_HEAD(&buf->list);
        if (size == 0)
                return 0;

        ANALYSIS_BEGIN(0);

        int left, min;
        //int coreid = __coreid();
        left = size;
        do {
                min = _min(left, BUFFER_SEG_SIZE);

#if 1
                seg = seg_huge_create(buf, min);
#else
                if (likely(coreid != -1)) {
                        seg = seg_huge_create(buf, min);
                } else {
                        seg = seg_sys_create(buf, min);
                }
#endif

                seg_add_tail(buf, seg);
                left -= min;
        } while (unlikely(left > 0));

        BUFFER_CHECK(buf);

        ANALYSIS_END(0, 1000 * 100, NULL);

        return 0;
}

inline int ltgbuf_init1(ltgbuf_t *buf, int size)
{
        seg_t *seg;

        LTG_ASSERT(size >= 0 && size < (1024 * 1024 * 100));

        buf->len = 0;
        buf->used = 0;
        INIT_LIST_HEAD(&buf->list);
        if (size == 0)
                return 0;

        ANALYSIS_BEGIN(0);

        int left, min;
        //int coreid = __coreid();
        left = size;
        do {
                min = _min(left, BUFFER_SEG_SIZE);
                seg = seg_sys_create(buf, min);
                seg_add_tail(buf, seg);
                left -= min;
        } while (unlikely(left > 0));

        BUFFER_CHECK(buf);

        ANALYSIS_END(0, 1000 * 100, NULL);

        return 0;
}

void IO_FUNC ltgbuf_free(ltgbuf_t *buf)
{
        BUFFER_CHECK(buf);

        if (unlikely(buf->len == 0))
                return;

        __ltgbuf_free1(buf);
}

int IO_FUNC ltgbuf_pop1(ltgbuf_t *buf, ltgbuf_t *newbuf, uint32_t len, int deep)
{
        struct list_head *pos, *n;
        seg_t *seg;
        uint32_t left, min;

        BUFFER_CHECK(buf);

        LTG_ASSERT(len <= buf->len);

        left = len;
        
        list_for_each_safe(pos, n, &buf->list) {
                seg = (seg_t *)pos;
                min = left < seg->len ? left : seg->len;

                if (min < seg->len) {
                        DBUG("pop %u from %u\n", min, seg->len);

                        if (newbuf) {
                                if (deep) {
                                        int ret = ltgbuf_appendmem(newbuf,
                                                                    seg->handler.ptr,
                                                                    min);
                                        if (unlikely(ret))
                                                UNIMPLEMENTED(__DUMP__);
                                } else {
                                        seg_t *newseg = seg->sop.seg_share(newbuf, seg);
                                        if (newseg == NULL) {
                                                UNIMPLEMENTED(__DUMP__);
                                        }

                                        newseg->len = min;
                                        seg_add_tail(newbuf, newseg);
                                }
                        }

                        seg->handler.ptr = seg->handler.ptr + min;
                        seg->handler.phyaddr += min;
                        seg->len -= min;
                } else {
                        list_del(pos);

                        LTG_ASSERT(min == seg->len);
                        if (newbuf) {
                                seg_add_tail(newbuf, seg);
                        } else {
                                seg->sop.seg_free(seg);
                        }
                }

                left -= min;
                if (left == 0)
                        break;
        }

        LTG_ASSERT(left == 0);

        buf->len -= len;

        BUFFER_CHECK(buf);
        if (newbuf) {
                BUFFER_CHECK(newbuf);
        }

        if (buf->len == 0) {
                buf->used = 0;
        }
        
        return 0;
}

int IO_FUNC ltgbuf_pop(ltgbuf_t *buf, ltgbuf_t *newbuf, uint32_t len)
{
        return ltgbuf_pop1(buf, newbuf, len, 0);
}

//copy a long mem into ltgbuf_t.
int ltgbuf_copy(ltgbuf_t *buf, const char *srcmem, int size)
{
        int ret, left, offset, min;

        LTG_ASSERT(size >= 0);
        BUFFER_CHECK(buf);

        left = size;
        offset = 0;
        while (left > 0) {
                min = left < BUFFER_SEG_SIZE ? left : BUFFER_SEG_SIZE;

                ret = ltgbuf_appendmem(buf, srcmem + offset, min);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                left -= min;
                offset += min;
        }

        BUFFER_CHECK(buf);

        return 0;
err_ret:
        return ret;
}

void ltgbuf_clone(ltgbuf_t *newbuf, const ltgbuf_t *buf)
{
        int ret;
        struct list_head *pos;
        seg_t *seg;

        BUFFER_CHECK(buf);
        BUFFER_CHECK(newbuf);
        LTG_ASSERT(newbuf->len == buf->len);

        list_for_each(pos, &buf->list) {
                seg = (seg_t *)pos;

                ret = ltgbuf_appendmem(newbuf, seg->handler.ptr, seg->len);
                if (ret)
                        UNIMPLEMENTED(__DUMP__);
        }

        BUFFER_CHECK(newbuf);
        BUFFER_CHECK(buf);
}

STATIC void __ltgbuf_iov_copy(seg_t *seg, struct iovec **_iov, int *_iov_count)
{
        int iov_count, left, count;
        struct iovec *iov = *_iov;
        void *ptr;

        iov_count = *_iov_count;
        LTG_ASSERT(iov_count > 0);
        left = seg->len;
        ptr = seg->handler.ptr;
        iov = *_iov;
        while (left) {
                count = left < (int)iov->iov_len ? left : (int)iov->iov_len;
                memcpy(ptr, iov->iov_base, count);
                ptr += count;
                left -= count;
                if (count == (int)iov->iov_len) {
                        iov++;
                        iov_count--;
                        LTG_ASSERT(iov_count >= 0);
                } else {
                        iov->iov_base += count;
                        iov->iov_len -= count;
                        LTG_ASSERT(left == 0);
                }
        }

        *_iov = iov;
        *_iov_count = iov_count;
}

#define IOV_MAX ((524288 * 2) / BUFFER_SEG_SIZE + 1)
#define IOV_ALLOC_MAX (1024 * 128)

void ltgbuf_clone1(ltgbuf_t *newbuf, const ltgbuf_t *buf, int init)
{
        int ret, iov_count;
        struct list_head *pos;
        struct iovec iov_array[IOV_MAX * 2], *iov_buf = NULL, *iov;

        BUFFER_CHECK(buf);

        if (init) {
                ltgbuf_init(newbuf, buf->len);
        } else {
                BUFFER_CHECK(newbuf);
                LTG_ASSERT(newbuf->len >= buf->len);
        }
        
        iov_count = IOV_MAX * 2;
        ret = ltgbuf_trans(iov_array, &iov_count, buf);
        if (ret != (int)buf->len) {
                ret = ltg_malloc((void **)&iov_buf, IOV_ALLOC_MAX);
                if (unlikely(ret))
                        LTG_ASSERT(0);

                iov_count = IOV_ALLOC_MAX / sizeof(struct iovec);
                ret = ltgbuf_trans(iov_buf, &iov_count, buf);
                LTG_ASSERT(ret == (int)buf->len);

                iov = iov_buf;
        } else {
                iov = iov_array;
        }

        int len = 0, left = buf->len;
        list_for_each(pos, &newbuf->list) {
                seg_t seg = *(seg_t *)pos;

                seg.len = _min(left, (int)seg.len);
                __ltgbuf_iov_copy(&seg, &iov, &iov_count);

                len += seg.len;
                left -= seg.len;

                if (left == 0)
                        break;

#if 0
                uint32_t crc1, crc2;
                crc1 = ltgbuf_crc(buf, 0, len);
                crc2 = ltgbuf_crc(newbuf, 0, len);
                LTG_ASSERT(crc1 == crc2);
#endif
        }

        BUFFER_CHECK(newbuf);
        BUFFER_CHECK(buf);
        //LTG_ASSERT(len == newbuf->len);
        if (iov_buf)
                ltg_free((void **)&iov_buf);


#if 0
        uint32_t crc1 = ltgbuf_crc(buf, 0, buf->len);
        uint32_t crc2 = ltgbuf_crc(newbuf, 0, newbuf->len);
        LTG_ASSERT(crc1 == crc2);
#endif
}

uint32_t ltgbuf_crc_stream(uint32_t *crcode, const ltgbuf_t *buf,
                            uint32_t offset, uint32_t size)
{
        uint32_t soff, count, left, step;
        struct list_head *pos;
        seg_t *seg;

        count = 0;
        left = size;

        list_for_each(pos, &buf->list) {
                seg = (seg_t *)pos;

                if (seg->len + count < offset) {
                        count += seg->len;
                        continue;
                }

                if (count < offset) {
                        soff = offset - count;
                        count += seg->len;
                } else
                        soff = 0;

                step = (seg->len - soff) < left ? (seg->len - soff) : left;

                DBUG("crc off %u size %u (%s) left %u\n", soff, step,
                      (char *)seg->handler.ptr + soff, left);

                crc32_stream(crcode, seg->handler.ptr + soff, step);

                left -= step;

                if (left == 0)
                        break;
        }

        return 0;
}

/*size is the length from the begin of buffer*/
uint32_t ltgbuf_crc(const ltgbuf_t *buf, uint32_t offset, uint32_t size)
{
        uint32_t crcode, crc;

        LTG_ASSERT(size <= buf->len);

        BUFFER_CHECK(buf);

        crc32_init(crcode);
        ltgbuf_crc_stream(&crcode, buf, offset, size);
        crc = crc32_stream_finish(crcode);

        DBUG("len %u off %u crc %x\n", buf->len, offset, crc);

        BUFFER_CHECK(buf);

        return crc;
}

int ltgbuf_appendzero(ltgbuf_t *buf, int size)
{
        int left, min;
        seg_t *seg;

        LTG_ASSERT(size >= 0);
        BUFFER_CHECK(buf);

        left = size;
        while (left > 0) {
                min = left < BUFFER_SEG_SIZE ? left : BUFFER_SEG_SIZE;

                seg = seg_huge_create(buf, min);
                LTG_ASSERT(seg);
                memset(seg->handler.ptr, 0x0, min);
                seg_add_tail(buf, seg);
                left -= min;
        }

        BUFFER_CHECK(buf);

        return 0;
}

int ltgbuf_writefile(const ltgbuf_t *buf, int fd, uint64_t offset)
{
        int ret, count, size;
        uint32_t trans_off = 0;
        struct iovec iov[10 * 1024 * 1024 / BUFFER_SEG_SIZE + 1];

        //DBUG("write fd %u off %llu size %llu\n", fd, (LLU)offset, (LLU)count);

        //LTG_ASSERT(buf->len <= BIG_BUF_LEN * 2);

        BUFFER_CHECK(buf);

        while (trans_off < buf->len) {
                count = 10 * 1024 * 1024 / BUFFER_SEG_SIZE + 1;
                size = ltgbuf_trans1(iov, &count, trans_off, buf);

                LTG_ASSERT(size);
                ret = pwritev(fd, iov, count, offset + trans_off);
                if (unlikely(ret < 0)) {
                        ret = errno;
                        GOTO(err_ret, ret);
                }

                trans_off += size;
        }

        LTG_ASSERT(trans_off == buf->len);

        return 0;
err_ret:
        return ret;
}

void *ltgbuf_head(ltgbuf_t *buf)
{
        BUFFER_CHECK(buf);

        LTG_ASSERT(buf->len);

        if (list_empty(&buf->list))
                return NULL;

        return ((seg_t *)buf->list.next)->handler.ptr;
}

int ltgbuf_trans(struct iovec *_iov, int *iov_count, const ltgbuf_t *buf)
{
        int seg_count, max, size = 0;
        struct list_head *pos;
        seg_t *seg;

        max = *iov_count;
        seg_count = 0;
        list_for_each(pos, &buf->list) {
                if (seg_count == max) {
                        break;
                }

                seg = (seg_t *)pos;
                LTG_ASSERT(seg->handler.ptr);

                _iov[seg_count].iov_len = seg->len;
                _iov[seg_count].iov_base = seg->handler.ptr;
                seg_count++;
                size += seg->len;
        }

        *iov_count = seg_count;
        //LTG_ASSERT(size == (int)buf->len);
        return size;
}

void ltgbuf_trans_addr(void **addr, const ltgbuf_t *buf)
{
        struct list_head *pos;
        seg_t *seg;
        int i = 0;
   
        list_for_each(pos, &buf->list) {
                seg = (seg_t *)pos;
                addr[i++] = seg->handler.ptr;
        }

        LTG_ASSERT(i < MAX_SGE);
}

int ltgbuf_trans1(struct iovec *_iov, int *iov_count, uint32_t offset, const ltgbuf_t *buf)
{
        int seg_count, max, size = 0;
        struct list_head *pos;
        seg_t *seg;

        max = *iov_count;
        seg_count = 0;
        list_for_each(pos, &buf->list) {
                if (seg_count == max) {
                        break;
                }

                seg = (seg_t *)pos;
                LTG_ASSERT(seg->handler.ptr);

                if (offset >= seg->len) {
                        offset -= seg->len;
                        continue;
                }

                _iov[seg_count].iov_len = seg->len - offset;
                _iov[seg_count].iov_base = seg->handler.ptr + offset;
                seg_count++;
                size += seg->len - offset;
                offset = 0;
        }

        *iov_count = seg_count;
        return size;
}

void ltgbuf_trans2(struct iovec *_iov, int *iov_count, uint32_t offset,
                    uint32_t size, const ltgbuf_t *buf)
{
        int seg_count;
        uint32_t rest;
        struct list_head *pos;
        seg_t *seg;

        seg_count = 0;
        list_for_each(pos, &buf->list) {
                seg = (seg_t *)pos;
                LTG_ASSERT(seg->handler.ptr);

                if (offset >= seg->len) {
                        offset -= seg->len;
                        continue;
                }

                rest = seg->len - offset;
                _iov[seg_count].iov_len = rest < size ? rest : size;
                _iov[seg_count].iov_base = seg->handler.ptr + offset;
                size -= _iov[seg_count].iov_len;
                offset = 0;
                seg_count++;

                if (!size) {
                        break;
                }
        }

        LTG_ASSERT(!size);

        *iov_count = seg_count;
}

STATIC void __ltgbuf_trans3(struct iovec *_iov, int *_iov_count, int iov_max,
                             size_t seglen, void *_ptr, size_t iovlen_max)
{
        size_t left, c;
        void *ptr;
        int iov_count;
        
        left = seglen;
        ptr = _ptr;
        iov_count = *_iov_count;
        while (left) {
                if (iov_count == iov_max) {
                        break;
                }
                
                c = left <= iovlen_max ? left : iovlen_max;

                _iov[iov_count].iov_base = ptr;
                _iov[iov_count].iov_len = c;

                left -= c;
                ptr += c;
                iov_count++;
        }

        *_iov_count = iov_count;
}

int ltgbuf_trans3(struct iovec *_iov, int *_iov_count, ltgbuf_t *buf, size_t iovlen_max)
{
        int iov_count, max, size = 0;
        struct list_head *pos;
        seg_t *seg;

        max = *_iov_count;
        iov_count = 0;
        list_for_each(pos, &buf->list) {
                if (iov_count == max) {
                        break;
                }

                seg = (seg_t *)pos;
                LTG_ASSERT(seg->handler.ptr);

                if (seg->len < iovlen_max) {
                        _iov[iov_count].iov_len = seg->len;
                        _iov[iov_count].iov_base = seg->handler.ptr;
                        iov_count++;
                } else {
                        __ltgbuf_trans3(_iov, &iov_count, max, seg->len,
                                         seg->handler.ptr, iovlen_max);
                }

                size += seg->len;
        }

        *_iov_count = iov_count;

        return size;
}

int ltgbuf_compress2(ltgbuf_t *buf, uint32_t max_seg_len)
{
        int ret;
        uint32_t idx1 = 0, idx2 = 0, len = 0, left, seg_left;
        struct list_head *pos, *n, list;
        seg_t *seg1, *seg2 = NULL;

        BUFFER_CHECK(buf);

        INIT_LIST_HEAD(&list);

        left = buf->len;
        list_for_each_safe(pos, n, &buf->list) {
                seg1 = (seg_t *)pos;
                seg_left = seg1->len;

                LTG_ASSERT(seg1->handler.ptr);

                while (seg_left) {
                        if (idx2 == 0) {
                                seg2 = seg_huge_create(buf, _min(left, max_seg_len));
                                if (seg2 == NULL) {
                                        ret = ENOMEM;
                                        GOTO(err_ret, ret);
                                }
                                list_add_tail(&seg2->hook, &list);
                        }

                        len = _min(seg1->len - idx1, seg2->len - idx2);

                        memcpy(seg2->handler.ptr + idx2, seg1->handler.ptr + idx1, len);

                        idx1 += len;
                        idx2 += len;

                        if (idx1 == seg1->len) {
                                idx1 = 0;
                                list_del(pos);
                                seg1->sop.seg_free(seg1);
                        }
                        if (idx2 == seg2->len) {
                                idx2 = 0;
                        }

                        left -= len;
                        seg_left -= len;
                }
        }

        list_splice_init(&list, &buf->list);

        BUFFER_CHECK(buf);

        return 0;
err_ret:
        return ret;
}

int ltgbuf_compress(ltgbuf_t *buf) {
        return ltgbuf_compress2(buf, BUFFER_SEG_SIZE);
}

STATIC int __ltgbuf_printhex(char *output, int outlen, const void *input, const int inlen)
{
        int ret, i;

        /*
         * format:
         * 00000000: 1300 0000 00f0 ed1e 1552 0500 c984 8c82
         * 00000010: 0000 c701 6673 6432 2020 2020 0000 0000
         */
        if (outlen < _ceil(inlen, 16) * 50) {
                ret = EINVAL;
                GOTO(err_ret, ret);
        }

        output[0] = '\0';
        for (i = 0; i < inlen; i++) {
                if (i % 16 == 0) {
                        sprintf(output + strlen(output), "%08x:", i);
                }

                if (i % 2 == 0) {
                        sprintf(output + strlen(output), " ");
                }

                sprintf(output + strlen(output), "%02x", *((unsigned char *)input + i));

                if ((i + 1) % 16 == 0 || i + 1 == inlen) {
                        sprintf(output + strlen(output), "\n");
                }
        }

        return 0;
err_ret:
        return ret;
}

int ltgbuf_dump(const ltgbuf_t *buf, uint32_t len, const char *s)
{
        int ret;
        uint32_t tmplen, hexlen;
        char *tmp, *hex;

        tmplen = _min(len, buf->len);
        hexlen = _ceil(tmplen, 16) * 50;

        ret = ltg_malloc((void **)&tmp, tmplen);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = ltg_malloc((void **)&hex, hexlen);
        if (unlikely(ret))
                GOTO(err_free, ret);

        ltgbuf_get(buf, tmp, tmplen);
        __ltgbuf_printhex(hex, hexlen, tmp, tmplen);

        if (s) {
                DINFO("%s buffer dump:%s\n", s, hex);
        } else {
                DINFO("buffer dump:%s\n", hex);
        }

        ltg_free((void **)&tmp);
        ltg_free((void **)&hex);

        return ret;
err_free:
        ltg_free((void **)&tmp);
err_ret:
        return ret;
}

STATIC void __get_new_size(uint32_t size, uint32_t *new_size,
                uint64_t offset, uint64_t *new_offset)
{
        uint32_t tmp_size = size;
        uint64_t tmp_off = offset;

        if ((offset % BLOCK_SIZE) != 0) {
                tmp_off = offset - offset % BLOCK_SIZE;
                tmp_size += offset - tmp_off;
        }

        if ((tmp_size % PAGE_SIZE) != 0) {
                tmp_size += PAGE_SIZE - (tmp_size % PAGE_SIZE);
        }

        *new_size = tmp_size;
        *new_offset = tmp_off;
}

int ltgbuf_nvme_init2(nvmeio_t *io, uint32_t size, uint64_t offset, ltgbuf_t *buf)
{
        seg_t *seg;

        DBUG("begin size %u offset %lu \n", size, offset);
        __get_new_size(size, &io->size, offset, &io->offset);
        DBUG("after size %u offset %lu\n", io->size, io->offset);
        ltgbuf_init(buf, io->size);
        seg = (seg_t *)buf->list.next;
        io->addr = seg->handler.ptr;
        io->phyaddr = seg->handler.phyaddr;

        return 0;
}

int ltgbuf_itor(const ltgbuf_t *buf, uint32_t size, off_t offset,
                 buf_itor_func func, void *ctx)
{
        int ret;
        struct list_head *pos;
        uint32_t rest, c;

        BUFFER_CHECK(buf);
        LTG_ASSERT(offset + size <= (off_t)buf->len);

        list_for_each(pos, &buf->list) {
                seg_t *seg = (seg_t *)pos;

                if (offset >= seg->len) {
                        offset -= seg->len;
                        continue;
                }

                rest = seg->len - offset;
                c = rest < size ? rest : size;
                ret = func(ctx, seg->handler.ptr + offset,
                           seg->handler.phyaddr + offset,
                           c);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                offset = 0;
                size -= c;
        }

        return 0;
err_ret:
        return ret;
}

void ltgbuf_bezero(ltgbuf_t *buf)
{
        ltgbuf_t tmp;
        ltgbuf_init(&tmp, 0);
        ltgbuf_appendzero(&tmp, buf->len);
        ltgbuf_clone1(buf, &tmp, 0);
        ltgbuf_free(&tmp);
}


void ltgbuf_check(const ltgbuf_t *buf)
{
        if (buf) {
                BUFFER_CHECK(buf);
        }
}
