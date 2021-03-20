#include <string.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_RPC

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

int S_LTG rpc_request_prep(ltgbuf_t *buf, const msgid_t *msgid, const void *request,
                           int reqlen, int replen, int datalen, int prog,
                           int priority, int coreid)
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

        net_req = ltgbuf_head1(buf, sizeof(*net_req));
        net_req->magic = LTG_MSG_MAGIC;
        net_req->len = sizeof(ltg_net_head_t) + reqlen;
        net_req->replen = replen;
        net_req->type = LTG_MSG_REQ;
        net_req->prog = prog;
        net_req->msgid = *msgid;
        net_req->crcode = 0;
        net_req->blocks = 0;
        net_req->coreid = coreid;
        net_req->master_magic = ltg_global.master_magic;
        net_req->status = 0;
        memcpy(net_req->buf, request, reqlen);

        net_req->blocks = datalen;
        
        return 0;
err_ret:
        return ret;
}
