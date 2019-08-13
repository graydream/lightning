

#define DBG_SUBSYS S_LTG_RPC

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_rpc.h"

int rpc_inited = 0;

int rpc_init(const char *name)
{
        int ret;

        (void) name;
        
        ret = net_init();
        if (ret)
                GOTO(err_ret, ret);

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
