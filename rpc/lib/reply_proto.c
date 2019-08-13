#include <string.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_RPC

#include "ltg_net.h"
#include "ltg_core.h"
#include "ltg_utils.h"
#include "ltg_rpc.h"

void stdrpc_reply_init_prep(const msgid_t *msgid, ltgbuf_t *buf, ltgbuf_t *data, int flag)
{
        int ret;
        ltg_net_head_t *net_rep;

        ret = ltgbuf_init(buf, sizeof(ltg_net_head_t));
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        net_rep = ltgbuf_head(buf);
        net_rep->magic = LTG_MSG_MAGIC;
        net_rep->len = sizeof(ltg_net_head_t);
        net_rep->type = LTG_MSG_REP;
        net_rep->prog = MSG_NULL;
        net_rep->msgid = *msgid;
        net_rep->crcode = 0;
        net_rep->blocks = 0;
        net_rep->load = core_latency_get();
        net_rep->time = gettime();
        net_rep->coreid = -1;

        if (data) {
                if (unlikely(flag)) {
                        net_rep->blocks = data->len;
                        net_rep->len += data->len;
                }
                ltgbuf_merge(buf, data);
        }
        
        DBUG("msgid %d.%d\n", net_rep->msgid.idx, net_rep->msgid.figerprint);
}

void stdrpc_reply_error_prep(const msgid_t *msgid, ltgbuf_t *buf, int _error)
{
        int ret;
        ltg_net_head_t *net_rep;
        ltg_net_err_t *net_err;
        uint32_t len;

        len = sizeof(ltg_net_err_t);

        ret = ltgbuf_init(buf, sizeof(ltg_net_head_t) + len);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        net_rep = ltgbuf_head(buf);
        net_rep->magic = LTG_MSG_MAGIC;
        net_rep->len = sizeof(ltg_net_head_t) + len;
        net_rep->type = LTG_MSG_REP;
        net_rep->prog = MSG_NULL;
        net_rep->msgid = *msgid;
        net_rep->crcode = 0;
        net_rep->blocks = 0;
        net_rep->load = core_latency_get();
        net_rep->time = gettime();
        net_rep->coreid = -1;

        net_err = (void *)net_rep->buf;

        net_err->magic = LTG_MSG_ERROR;
        net_err->err = _error;
}

void stdrpc_reply_error(const sockid_t *sockid, const msgid_t *msgid, int _error)
{
        int ret;
        ltg_net_head_t *net_rep;
        ltg_net_err_t *net_err;
        uint32_t len;
        ltgbuf_t buf;
        net_handle_t nh;

        len = sizeof(ltg_net_err_t);

        ret = ltgbuf_init(&buf, sizeof(ltg_net_head_t) + len);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        net_rep = ltgbuf_head(&buf);
        net_rep->magic = LTG_MSG_MAGIC;
        net_rep->len = sizeof(ltg_net_head_t) + len;
        net_rep->type = LTG_MSG_REP;
        net_rep->prog = MSG_NULL;
        net_rep->msgid = *msgid;
        net_rep->crcode = 0;
        net_rep->blocks = 0;
        net_rep->load = core_latency_get();
        net_rep->time = gettime();
        net_rep->coreid = -1;

        net_err = (void *)net_rep->buf;

        net_err->magic = LTG_MSG_ERROR;
        net_err->err = _error;

        sock2nh(&nh, sockid);
        sdevent_queue(&nh, &buf);

        ltgbuf_free(&buf);
}
