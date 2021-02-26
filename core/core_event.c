#include <limits.h>
#include <time.h>
#include <string.h>
#include <sys/epoll.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <netinet/tcp.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_net.h"
#include "ltg_utils.h"
#include "ltg_core.h"
#include "ltg_net.h"

extern int ltg_nofile_max;

static void __core_event_poller(void *_core, void *var, void *_corenet)
{
        int ret, count;
        struct pollfd pfd[1];
        core_t *core = _core;

        (void) var;
        (void) _corenet;
        
        LTG_ASSERT(core->interrupt_eventfd != -1);

        pfd[0].fd = core->interrupt_eventfd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        count = 1;

        while (1) {
                ret = poll(pfd, count, 1000);
                if (ret < 0)  {
                        ret = errno;
                        if (ret == EINTR) {
                                continue;
                        } else
                                GOTO(err_ret, ret);
                }

                count = ret;
                break;
        }

        DBUG("got event %d\n", count);

        uint64_t left;

        if (count) {
                ret = read(core->interrupt_eventfd, &left, sizeof(left));
                if (unlikely(ret < 0)) {
                        ret = errno;
                        GOTO(err_ret, ret);
                }
        }
        
        return;
err_ret:
        UNIMPLEMENTED(__DUMP__);
        return;
}

static int __core_event_init(va_list ap)
{
        int ret;

        va_end(ap);

        ret = core_register_poller("core_event_poller", __core_event_poller, NULL);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}
        
int core_event_init(uint64_t mask)
{
        int ret;

        ret = core_init_modules1("corenet", mask, __core_event_init, NULL);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}
