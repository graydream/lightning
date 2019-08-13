#include <string.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_RPC

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

rpc_table_t *corerpc_self()
{
        return core_tls_get(VARIABLE_CORERPC);
}

rpc_table_t *corerpc_self_byctx(void *ctx)
{
        return core_tls_getfrom1(ctx, VARIABLE_CORERPC);
}

static int __corerpc_init__(const char *name, core_t *core, rpc_table_t **_rpc_table)
{
        int ret;
        rpc_table_t *__rpc_table_private__;

        ret = rpc_table_init(name, &__rpc_table_private__, 1);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        core_tls_set(VARIABLE_CORERPC, __rpc_table_private__);
        core->rpc_table = __rpc_table_private__;
        *_rpc_table = __rpc_table_private__;

        return 0;
err_ret:
        return ret;
}

void corerpc_scan(void *ctx)
{
        rpc_table_t *__rpc_table_private__ = corerpc_self_byctx(ctx);

        if (likely(__rpc_table_private__)) {
#if 1
                rpc_table_scan(__rpc_table_private__, _min(ltgconf.rpc_timeout, 10), 1);
                sche_run(core_tls_getfrom1(ctx, VARIABLE_SCHEDULE));
#else
                rpc_table_scan(__rpc_table_private__, _min(ltgconf.rpc_timeout, 10), 0);
#endif
        }
}

#if ENABLE_RDMA
void corerpc_rdma_reset(const sockid_t *sockid)
{
        rpc_table_t *__rpc_table_private__ = NULL;

        __rpc_table_private__ = corerpc_self();

        if (__rpc_table_private__) {
                DINFO("rpc table reset ... \n");
                rpc_table_reset(__rpc_table_private__, sockid, NULL);
        } else
                LTG_ASSERT(0);
}
#endif

rpc_table_t *corerpc_self_byctx(void *ctx);
rpc_table_t *corerpc_self();

void corerpc_reset(const sockid_t *sockid)
{
        rpc_table_t *__rpc_table_private__ = corerpc_self();

        if (__rpc_table_private__) {
                rpc_table_reset(__rpc_table_private__, sockid, NULL);
        }
}

inline static void __corerpc_scan(void *_core, void *var, void *_corerpc)
{
        (void) _core;
        (void) _corerpc;

        corerpc_scan(var);

        return;
}

inline static void __corerpc_destroy(void *_core, void *var, void *_corerpc)
{
        core_t *core = _core;

        (void) _corerpc;
        (void) var;

        corerpc_destroy((void *)&core->rpc_table);

        return;
}

static int __corerpc_init(va_list ap)
{
        int ret;
        core_t *core = core_self();
        char name[MAX_NAME_LEN];
        rpc_table_t *rpc_table;

        va_end(ap);

        snprintf(name, sizeof(name), "%s[%u]", core->name, core->hash);
        ret = __corerpc_init__(name, core, &rpc_table);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = core_register_scan("corerpc_scan", __corerpc_scan, rpc_table);
        if (unlikely(ret))
                GOTO(err_destroy, ret);

        DINFO("%s[%u] rpc inited\n", core->name, core->hash);

        return 0;
err_destroy:
        UNIMPLEMENTED(__DUMP__);
err_ret:
        return ret;
}

int corerpc_init()
{
        int ret;

        ret = core_init_modules("corerpc", __corerpc_init, NULL);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}
