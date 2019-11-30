#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_utils.h"
#include "ltg_net.h"

inline int sock_accept(net_handle_t *nh, int srv_sd, int tuning,
                       int nonblock)
{
        return tcp_sock_accept(nh, srv_sd, tuning, nonblock);
}

inline int sock_getinfo(uint32_t *info_count, sock_info_t *info,
                        uint32_t info_count_max, uint32_t port,
                        const ltg_netconf_t *filter)
{
        return tcp_sock_getaddr(info_count, info, info_count_max, port, filter);
}

inline int sock_setblock(int sd)
{
        int ret, flags;

        flags = fcntl(sd, F_GETFL);
        if (flags == -1) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        if (flags & O_NONBLOCK) {
                flags = flags ^ O_NONBLOCK;

                ret = fcntl(sd, F_SETFL, flags);
                if (ret == -1) {
                        ret = errno;
                        GOTO(err_ret, ret);
                }
        }

        return 0;
err_ret:
        return ret;
}

inline int sock_setnonblock(int sd)
{
        int ret, flags;

        flags = fcntl(sd, F_GETFL);
        if (flags == -1) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        if ((flags & O_NONBLOCK) == 0) {
                flags = flags | O_NONBLOCK;
                ret = fcntl(sd, F_SETFL, flags);
                if (ret == -1) {
                        ret = errno;
                        GOTO(err_ret, ret);
                }
        }

        return 0;
err_ret:
        return ret;
}

inline int sock_checknonblock(int sd)
{
        int ret, flags;

        flags = fcntl(sd, F_GETFL);
        if (flags == -1) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        LTG_ASSERT(flags & O_NONBLOCK);

        return 0;
err_ret:
        return ret;
}
