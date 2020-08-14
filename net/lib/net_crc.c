#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_utils.h"
#include "ltg_net.h"

#define LNET_NET_REQ_OFF (sizeof(uint32_t) * 3)

int ltgnet_pack_crcsum(ltgbuf_t *pack)
{
        uint32_t crcode;
        ltg_net_head_t *head;

        head = ltgbuf_head1(pack, sizeof(*head));

        if (head->crcode)
                return 0;

        crcode = ltgbuf_crc(pack, LNET_NET_REQ_OFF, pack->len);

        head->crcode = crcode;

        DBUG("CRC %x len %u type %u prog %u seq %u no %u\n", crcode, head->len,
             head->type, head->prog, head->msgid.figerprint, head->msgid.idx);

        return 0;
}

int ltgnet_pack_crcverify(ltgbuf_t *pack)
{
        int ret;
        uint32_t crcode;
        ltg_net_head_t head;

        ltgbuf_get(pack, &head, sizeof(ltg_net_head_t));

        if (!head.crcode)
                return 0;

        crcode = ltgbuf_crc(pack, LNET_NET_REQ_OFF, pack->len);

        if (head.crcode != crcode) {
                DERROR("crc code error %x:%x len %u\n", head.crcode,
                       crcode, pack->len);
                ret = EBADF;
                UNIMPLEMENTED(__DUMP__);
                GOTO(err_ret, ret);
        }

        DBUG("CRC %x len %u type %u prog %u seq %u no %u\n", crcode, head.len,
             head.type, head.prog, head.msgid.figerprint, head.msgid.idx);

        return 0;
err_ret:
        return ret;
}
