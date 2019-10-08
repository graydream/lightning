#include <limits.h>
#include <time.h>
#include <string.h>
#include <sys/epoll.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <stdarg.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_CORE

#include "ltg_net.h"
#include "ltg_utils.h"
#include "ltg_rpc.h"
#include "ltg_core.h"
#include "ltg_net.h"

typedef struct {
        time_t last_update;
        uint64_t used;
        uint64_t count;
} core_latency_t;

typedef struct {
        struct list_head hook;
        uint64_t used;
        uint64_t count;
} core_latency_update_t;

typedef struct {
        ltg_spinlock_t lock;
        struct list_head list;
        uint64_t count;
        uint64_t used;
        double last_result;
} core_latency_list_t;

static __thread core_latency_t *core_latency = NULL;
static core_latency_list_t *core_latency_list;

static int __core_latency_private_init(core_latency_t **_core_latency)
{
        int ret;

        LTG_ASSERT(core_latency == NULL);
        ret = ltg_malloc((void **)&core_latency, sizeof(*core_latency));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        memset(core_latency, 0x0, sizeof(*core_latency));
        *_core_latency = core_latency;

        return 0;
err_ret:
        return ret;
}

static void __core_latency_private_destroy()
{
        LTG_ASSERT(core_latency);
        ltg_free((void **)&core_latency);
}

static int __nodectl_set(const char *key, const char *value)
{
        int ret;
        char path[MAX_PATH_LEN];

        snprintf(path, MAX_PATH_LEN, "/dev/shm/%s/latency/%s", ltgconf_global.system_name, key);

        ret = _set_value(path, value, strlen(value) + 1, O_CREAT | O_TRUNC);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

static int __core_latency_worker__()
{
        int ret;
        struct list_head list, *pos, *n;
        core_latency_update_t *core_latency_update;
        char path[MAX_PATH_LEN], buf[MAX_BUF_LEN];
        double latency;

        INIT_LIST_HEAD(&list);

        ret = ltg_spin_lock(&core_latency_list->lock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        list_splice_init(&core_latency_list->list, &list);

        ltg_spin_unlock(&core_latency_list->lock);

        list_for_each_safe(pos, n, &list) {
                list_del(pos);
                core_latency_update = (void *)pos;
                core_latency_list->used += core_latency_update->used;
                core_latency_list->count += core_latency_update->count;
                ltg_free((void **)&pos);
        }

        if (core_latency_list->count) {
                core_latency_list->last_result
                        = ((double)(core_latency_list->used + core_latency_list->last_result)
                           / (core_latency_list->count + 1));
        } else
                core_latency_list->last_result /= 2;

        latency = core_latency_list->last_result / (1000);
        core_latency_list->used = 0;
        core_latency_list->count = 0;

        snprintf(path, MAX_PATH_LEN, "latency/10");
        snprintf(buf, MAX_PATH_LEN, "%fms\n", latency);
        //DINFO("latency %s", buf);

        DBUG("latency %llu\n", (LLU)core_latency_list->last_result);

        __nodectl_set(path, buf);

        return 0;
err_ret:
        return ret;
}

static void *__core_latency_worker(void *arg)
{
        int ret;

        (void) arg;

        while (1) {
                sleep(4);

                ret = __core_latency_worker__();
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);
        }

        return NULL;
}

static int __core_latency_init__()
{
        int ret;

        LTG_ASSERT(core_latency_list == NULL);
        ret = ltg_malloc((void **)&core_latency_list, sizeof(*core_latency_list));
        if (unlikely(ret))
                GOTO(err_ret, ret);


        INIT_LIST_HEAD(&core_latency_list->list);

        ret = ltg_spin_init(&core_latency_list->lock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = ltg_thread_create(__core_latency_worker, NULL, "__core_latency_worker");
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

static int __core_latency_update()
{
        int ret;
        time_t now = gettime();
        core_latency_update_t *core_latency_update;
        core_t *core;

        if (now - core_latency->last_update < 2) {
                return 0;
        }

        core = core_self();
        DBUG("%s update latency\n", core->name);

        ret = ltg_malloc((void **)&core_latency_update, sizeof(*core_latency_update));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        core_latency_update->used = core_latency->used;
        core_latency_update->count = core_latency->count;
        core_latency->used = core_latency->used / core_latency->count;
        core_latency->count = 1;
        core_latency->last_update = now;

        ret = ltg_spin_lock(&core_latency_list->lock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        list_add_tail(&core_latency_update->hook, &core_latency_list->list);

        ltg_spin_unlock(&core_latency_list->lock);

        return 0;
err_ret:
        return ret;
}

void core_latency_update(uint64_t used)
{
        if (unlikely(core_latency == NULL)) {
                return;
        }

        core_latency->used += used;
        core_latency->count++;

        DBUG("latency %llu / %llu\n", (LLU)core_latency->used, (LLU)core_latency->count);
        __core_latency_update();
}

uint64_t IO_FUNC core_latency_get()
{
        if (core_latency && core_latency->count) {
                DBUG("latency %llu / %llu\n", (LLU)core_latency->used, (LLU)core_latency->count);
                return core_latency->used / core_latency->count;
        } else if (core_latency_list) {
                DBUG("latency %llu\n", (LLU)core_latency_list->last_result);
                return core_latency_list->last_result;
        } else
                return 0;
}

#if 0
inline static void __core_latency_routine(void *_core, void *var, void *_core_latency)
{
        (void) var;
        (void) _core;
        (void) _core_latency;

        return;
}
#endif

inline static void __core_latency_destroy(void *_core, void *var, void *_core_latency)
{
        core_t *core = _core;

        (void) core;
        (void) _core_latency;
        (void) var;

        __core_latency_private_destroy();

        return;
}

static int __core_latency_init(va_list ap)
{
        int ret;
        core_t *core = core_self();
        core_latency_t *core_latency;

        va_end(ap);

        ret = __core_latency_private_init(&core_latency);
        if (unlikely(ret))
                GOTO(err_ret, ret);

#if 0
        ret = core_register_routine("core_latency", __core_latency_routine, core_latency);
        if (unlikely(ret))
                GOTO(err_destroy, ret);
#endif

        ret = core_register_destroy("core_latency", __core_latency_destroy, core_latency);
        if (unlikely(ret))
                GOTO(err_destroy, ret);

        DINFO("%s[%u] latency inited\n", core->name, core->hash);

        return 0;
err_destroy:
        UNIMPLEMENTED(__DUMP__);
err_ret:
        return ret;
}

int core_latency_init()
{
        int ret;

        ret = __core_latency_init__();
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        ret = core_init_modules("core_latency", __core_latency_init, NULL);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        DINFO("core global latency inited\n");

        return 0;
err_ret:
        return ret;
}
