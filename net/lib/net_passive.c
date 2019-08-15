#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_core.h"

static void __net_checkinfo(const sock_info_t *sock, int count)
{
        int i;

        if (ltgconf.daemon) {
                LTG_ASSERT(count);
        }

        if (count == 1)
                return;

        //LTG_ASSERT(count > 1);

        for (i = 1; i < count; i++) {
                if (sock[i].addr == sock[0].addr && sock[i].port == sock[0].port) {
                        LTG_ASSERT(0);
                }
        }

        if (count > 2)
                __net_checkinfo(&sock[1], count - 1);
}

//static int __net_getinfo(char *infobuf, uint32_t *infobuflen, uint32_t port)
int net_getinfo(char *infobuf, uint32_t *infobuflen, uint32_t port)
{
        int ret;
        ltg_net_info_t *info;
        uint32_t info_count_max, count = 0;
        char hostname[MAX_NAME_LEN];

        info = (ltg_net_info_t *)infobuf;
        memset(infobuf, 0x0, sizeof(ltg_net_info_t));
        info->info_count = 0;

        if (port != (uint32_t)-1) {
                info_count_max = (*infobuflen - sizeof(ltg_net_info_t))
                        / sizeof(sock_info_t);

                ret = sock_getinfo(&count, info->info,
                                    info_count_max, port);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }

                info->info_count = count;
                        
                LTG_ASSERT(strlen(ltgconf.service_name));
                if (ltgconf.daemon) {
                        LTG_ASSERT(count);
                }
        }

        if (ltgconf.daemon) {
                LTG_ASSERT(count);
        }
                
        info->id = *net_getnid();
        info->magic = LTG_MSG_MAGIC;
        info->uptime = gettime();

        ret = gethostname(hostname, MAX_NAME_LEN);
        if (unlikely(ret < 0)) {
                ret = errno;
                GOTO(err_ret, ret);
        }
                
        snprintf(info->name, MAX_NAME_LEN, "%s:%s", hostname, ltgconf.service_name);

        DBUG("info.name %s\n", info->name);

        if (port != LNET_PORT_NULL)
                LTG_ASSERT(info->info_count);

        *infobuflen = sizeof(ltg_net_info_t)
                + sizeof(sock_info_t) * info->info_count;

        info->len = *infobuflen;

        if (strcmp(info->name, "none") == 0) {
                ret = EAGAIN;
                GOTO(err_ret, ret);
        }

        DBUG("local nid "NID_FORMAT" info_count %u port %u\n",
             NID_ARG(&info->id), info->info_count, port);

        __net_checkinfo(info->info, info->info_count);

        return 0;
err_ret:
        return ret;
}

#if 0
static char __info__[MAX_BUF_LEN];
static time_t __last_update__ = 0;

int net_getinfo(char *infobuf, uint32_t *infobuflen, uint32_t port)
{
        int ret;
        time_t now = gettime();
        ltg_net_info_t *info = __info__;

        if (now - __last_update__ > 10) {
                uint32_t buflen = MAX_BUF_LEN
                ret = __net_getinfo(__info__, &buflen, port);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }

                memcpy(infobuf, info, info->len);
                goto out;
        }
        
        
out:
        return 0;
err_ret:
        return ret;
}
#endif
