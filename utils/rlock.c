#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DBG_SUBSYS S_LTG_UTILS

#include "ltg_list.h"
#include "rlock.h"
#include "sche.h"

uint64_t rlock_size(uint64_t max_pos) { return max_pos / 8; }

void rlock_destory(rlock_t **_rl)
{
        rlock_t *rl = *_rl;
        free(rl->array);
        pspin_destroy(&rl->lock);

        ltg_free((void **)&rl);
        *_rl = NULL;
        
}

static int __rlock_create(rlock_t **_rl, uint64_t size)
{
        int ret;
        rlock_t *rl;

        ret = ltg_malloc((void **)&rl, sizeof(*rl));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        memset(rl, 0x0, sizeof(*rl));
        
        rl->size = size;
        rl->array = (uint8_t *)malloc(size);
        if(!rl->array) {
                ret = ENOMEM;
                GOTO(err_free, ret);
        }

        memset(rl->array, 0, size);
        INIT_LIST_HEAD(&rl->lock_list);
        pspin_init(&rl->lock);

        *_rl = rl;
        
        return 0;
err_free:
        ltg_free((void **)&rl);
err_ret:
        return ret;
}

int rlock_is_set(rlock_t *rl, uint64_t pos)
{
        int l_index = pos / 8;
        int r_index = pos % 8;

        return rl->array[l_index] & (1 << r_index);
}

void rlock_clear(rlock_t *rl, uint64_t pos, uint32_t count)
{
        for (int i = 0; i < (int)count; i++) {
                int l_index = pos / 8;
                int r_index = pos % 8;

                rl->array[l_index] &= ~(1 << r_index);

                pos++;
        }
}

void rlock_set(rlock_t *rl, uint64_t pos, uint32_t count)
{
        for (int i = 0; i < (int)count; i++) {
                int l_index = pos / 8;
                int r_index = pos % 8;

                rl->array[l_index] |= 1 << r_index;

                pos++;
        }
}


static void __align(uint32_t ps, uint64_t *_off2, uint32_t *_size2) {
        uint64_t off1 = *_off2, off2;
        uint32_t size1 = *_size2, size2;
                
        off2 = off1;    
        size2 = size1;

        if (off1 % ps) {
                off2 -= off1 % ps;
                size2 += off1 % ps;
        }

        if (size2 % ps) {
                size2 += ps - size2 % ps;
        }
                
        *_off2 = off2;
        *_size2 = size2;
}                       


static int __rlock_unlock(rlock_t *rl, uint64_t pos)
{
        struct list_head *i;
        struct list_head *n;

        assert(pos < rl->size * 8);
        pspin_lock(&rl->lock);

        assert(rlock_is_set(rl, pos));

        DBUG("unlock: *%p, pos:%d\r\n", rl, pos);

        list_for_each_safe(i, n, &rl->lock_list) {
                rlock_entry_t *entry = (rlock_entry_t *)i;
                
                if(entry->pos == pos) {
                        list_del_init(&entry->list_entry);
                        sche_task_post(&entry->task, 0, NULL);

                        free(entry);

                        DBUG("unlock: *%p, resume\r\n", rl);
                        pspin_unlock(&rl->lock);
                        return 0;
                }
        }

        DBUG("unlock: *%p, clear\r\n", rl);
        rlock_clear(rl, pos, 1);
        
        pspin_unlock(&rl->lock);
        return 0;
}

static int __rlock_lock(rlock_t *rl, uint64_t pos)
{
        int ret;

        assert(pos < rl->size * 8);

        ret = pspin_lock(&rl->lock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        DBUG("lock: *%p, pos:%d\r\n", rl, pos);
        if (rlock_is_set(rl, pos)) {
                rlock_entry_t *entry = malloc(sizeof(rlock_entry_t));
                entry->pos = pos;
                entry->task = sche_task_get();
                list_add_tail(&entry->list_entry, &rl->lock_list);

                DBUG("lock insert: %p\r\n", entry->task);
                
                pspin_unlock(&rl->lock);
                /*todo. is it a prorlem here??*/
                sche_yield("rlock", NULL, NULL);
                assert(rlock_is_set(rl, pos));
        } else {
                rlock_set(rl, pos, 1);

                DBUG("lock set: *%p\r\n", rl);
                pspin_unlock(&rl->lock);
        }

        return 0;
err_ret:
        return ret;
}

int rlock_lock(rlock_t *rl, uint64_t offset, uint32_t size)
{
        __align(PAGE_SIZE, &offset, &size);

        for(uint32_t i = 0; i < size / PAGE_SIZE;i++)
                __rlock_lock(rl, offset / PAGE_SIZE + i);

        return 0;
}

void rlock_unlock(rlock_t *rl, uint64_t offset, uint32_t size)
{
        __align(PAGE_SIZE, &offset, &size);

        for(uint32_t i = 0; i < size / PAGE_SIZE;i++)
                __rlock_unlock(rl, offset / PAGE_SIZE + i);
}

int rlock_init(rlock_t **rl, uint32_t size)
{
        int ret;

        ret = __rlock_create(rl, size / PAGE_SIZE / 8);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}
