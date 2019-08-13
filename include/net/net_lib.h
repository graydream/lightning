#ifndef __LNET_NET_H__
#define __LNET_NET_H__

#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ltg_net.h"
#include "sdevent.h"
#include "ltg_utils.h"

#define LTG_MSG_MAGIC   0x866aa9f0
#define LTG_MSG_ERROR   0x1c3af910
#define MAX_NODEID_LEN 128

typedef enum {
        LTG_MSG_REQ = 0x01,
        LTG_MSG_REP = 0x04,
} net_msgtype_t;

#pragma pack(8)

typedef struct  {
        uint32_t magic;
        uint32_t len;
        uint32_t blocks;
        uint32_t prog;
        msgid_t msgid;
        uint32_t type;
        uint32_t crcode;     /* crc code of following data */
        uint32_t time;
        uint32_t group;
        uint32_t coreid;
        uint32_t master_magic;
        uint64_t load;
        char buf[0];
} ltg_net_head_t ;

/**
 * @note persist in etcd
 */

typedef struct {
        uint32_t len; /*length of the info*/
        uint32_t uptime;
        nid_t id;
        char name[MAX_NODEID_LEN];
        uint32_t magic;
        uint16_t info_count;       /**< network interface number */
        sock_info_t info[0];  /**< host byte order */
} ltg_net_info_t;

#pragma pack()

#define NET_HEAD_DUMP(head) do {                                \
        DBUG("head %p magic %x type %d len %d msg (%d %x) data size %d blocks %d\n", \
                (head),                                         \
                (head)->magic,                                  \
                (head)->type,                                   \
                (head)->len,                                    \
                (head)->msgid.idx,                              \
                (head)->msgid.figerprint,                       \
                (head)->msgid.data_prop.size,                   \
                (head)->blocks);                                \
} while (0)

int ltgnet_pack_crcsum(ltgbuf_t *pack);
int ltgnet_pack_crcverify(ltgbuf_t *pack);

typedef struct {
        uint32_t magic;
        uint32_t err;
} ltg_net_err_t;

static inline int ltg_pack_err(ltgbuf_t *pack)
{
        ltg_net_err_t net_err;

        if (pack->len == sizeof(ltg_net_err_t)) {
                ltgbuf_get(pack, &net_err, sizeof(ltg_net_err_t));
                if (net_err.magic == LTG_MSG_ERROR)
                        return net_err.err;

                return 0;
        }

        return 0;
}

/* net_lib.c */
int net_init();
int net_destroy(void);

/* net_passive.c */
int net_getinfo(char *infobuf, uint32_t *infobuflen, uint32_t port);


#endif
