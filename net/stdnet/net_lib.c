

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_rpc.h"

net_global_t ng;

extern int ltg_nofile_max;

int net_init(net_proto_t *op)
{
        int ret;
	//int ksubversion;

        //LTG_ASSERT(NET_HANDLE_LEN >= sizeof(net_handle_t));

        if (op)
                ng.op = *op;

        ng.op.head_len = sizeof(ltg_net_head_t);
        ng.op.writer = ng.op.writer ? ng.op.writer
                : net_events_handle_write;
        ng.op.reader = ng.op.reader ? ng.op.reader
                : net_events_handle_read;
        ng.op.pack_len = ng.op.pack_len ? ng.op.pack_len
                : rpc_pack_len;
        ng.op.pack_handler = ng.op.pack_handler ? ng.op.pack_handler
                : rpc_pack_handler;

        ret = netable_init(ltgconf.daemon);
        if (ret)
                GOTO(err_ret, ret);

        ret = sdevent_init(ltg_nofile_max);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int net_destroy(void)
{
        return 0;
}
