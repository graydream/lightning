#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>
#include <errno.h>
#include <sys/statfs.h>

#define DBG_SUBSYS S_LTG_UTILS

#include "ltg_lib.h"
#include "qd.h"

static __thread struct list_head __qd_list__;

typedef struct {
        struct list_head hook;
        char name[MAX_NAME_LEN];
        void *ctx;
        qd_func_t func;
} qd_entry_t;

static void __qd_dump(struct list_head *list)
{
        struct list_head *pos; 
        qd_entry_t *ent;
        char buf[MAX_BUF_LEN];
        int count = 0, size, qd;

        buf[0] = '\0';
        list_for_each(pos, list) {
                ent = (void *)pos;

                qd = ent->func(ent->ctx);

                size = strlen(buf);
                snprintf(buf + size, MAX_BUF_LEN - size, "%s %d ", ent->name, qd);
                count += qd;
        }

        if (count) {
                core_t *core = core_self();
                DINFO("%s[%d] qd: %s\n", core->name, core->hash, buf);
        } else {
                DBUG("queue depth null\n");
        }
}

static void __qd_task(void *_list)
{
        struct list_head *list = _list;

        while (1) {
                sche_task_sleep("qd sleep", 1000 * 1000 * 2);

                __qd_dump(list);
        }
}

static int __qd_init(va_list ap)
{
        (void) ap;
        
        INIT_LIST_HEAD(&__qd_list__);

        sche_task_new("queue depth", __qd_task, &__qd_list__, -1);
        
        return 0;
}


int qd_init()
{
        int ret;

        ret = core_init_modules("queue depth", __qd_init, NULL);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

void qd_register(void *ctx, char *name, qd_func_t func)
{
        qd_entry_t *ent = slab_static_alloc(sizeof(*ent));

        ent->ctx = ctx;
        strcpy(ent->name, name);
        ent->func = func;

        list_add_tail(&ent->hook, &__qd_list__);
}
