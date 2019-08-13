#include <string.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_RPC

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

int rpc_request_prep(ltgbuf_t *buf, const msgid_t *msgid, const void *request,
                     int reqlen, const ltgbuf_t *data, int prog, int merge, int priority)
{
        int ret;
        ltg_net_head_t *net_req;

        (void) priority;
        
        if (unlikely(ltg_global.master_magic == (uint32_t)-1)) {
                ret = ENOSYS;
                GOTO(err_ret, ret);
        }
        
        if (unlikely(reqlen + sizeof(ltg_net_head_t) > RDMA_MESSAGE_SIZE)) {
                DERROR("why we get such a request? %u\n", reqlen);
                LTG_ASSERT(0);
        }
        
        ret = ltgbuf_init(buf, sizeof(ltg_net_head_t) + reqlen);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        net_req = ltgbuf_head(buf);
        net_req->magic = LTG_MSG_MAGIC;
        net_req->len = sizeof(ltg_net_head_t) + reqlen;
        net_req->type = LTG_MSG_REQ;
        net_req->prog = prog;
        net_req->msgid = *msgid;
        net_req->crcode = 0;
        net_req->blocks = 0;
        net_req->coreid = -1;
        net_req->group = priority;
        net_req->master_magic = ltg_global.master_magic;
        net_req->load = core_latency_get();
        memcpy(net_req->buf, request, reqlen);

        if (data) {
                net_req->blocks = data->len;
                
                if (unlikely(merge)) {
                        net_req->len += data->len;
                        ltgbuf_t tmp;
                        ltgbuf_init(&tmp, 0);
                        ltgbuf_reference(&tmp, data);
                        ltgbuf_merge(buf, &tmp);
                }
        }

        return 0;
err_ret:
        return ret;
}
