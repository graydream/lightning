

#define DBG_SUBSYS S_LTG_RPC

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_rpc.h"

int rpc_inited = 0;

int rpc_init(net_proto_t *op, const char *name, int seq, const char *path)
{
        int ret;

        (void) path;
        
#if 0
        if (!op->request_handler) {
                DERROR("you need a request handler, halt\n");

                LTG_ASSERT(0);
        }
#endif
        ret = net_init(op);
        if (ret)
                GOTO(err_ret, ret);

        if (strlen(name) >= MAX_NAME_LEN) {
                ret = EINVAL;
                GOTO(err_ret, ret);
        }

        if (seq != -1)
                ng.seq = seq;

        strcpy(ng.name, name);

        ret = net_rpc_init();
        if (ret)
                GOTO(err_ret, ret);
        
        ret = rpc_table_init("default", &__rpc_table__, 0);
        if (ret)
                GOTO(err_ret, ret);

        rpc_inited = 1;

        return 0;
err_ret:
        return ret;
}

int rpc_destroy(void)
{
        int ret;

        DBUG("wait for net destroy...\n");

        rpc_inited = 0;

        ret = net_destroy();
        if (ret)
                GOTO(err_ret, ret);

        DINFO("net destroyed\n");

        return 0;
err_ret:
        return ret;
}
