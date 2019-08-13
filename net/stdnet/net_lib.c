

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_rpc.h"

extern int ltg_nofile_max;
net_proto_t net_proto;

int net_init()
{
        int ret;

        net_proto.head_len = sizeof(ltg_net_head_t);
        net_proto.writer = net_proto.writer ? net_proto.writer
                : net_events_handle_write;
        net_proto.reader = net_proto.reader ? net_proto.reader
                : net_events_handle_read;
        net_proto.pack_len = net_proto.pack_len ? net_proto.pack_len
                : rpc_pack_len;
        net_proto.pack_handler = net_proto.pack_handler ? net_proto.pack_handler
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
