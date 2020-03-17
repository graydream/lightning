#include <errno.h>

#define DBG_SUBSYS S_LTG_UTILS

#include "ltg_net.h"
#include "3part.h"
#include "utils/ltg_conf.h"
#include "ltg_utils.h"
#include "ltg_core.h"

#define ERRNO_MAX 1024

static int __errno_idx__ = 0;
static char *__errno__[ERRNO_MAX] = {0};

int ltg_errno_set(int idx, const char *_str)
{
        int ret;

        if (__errno_idx__ == ERRNO_MAX) {
                ret = ENOSPC;
                GOTO(err_ret, ret);
        }

        if (__errno__[idx]) {
                ret = EEXIST;
                GOTO(err_ret, ret);
        }

        char *str;
        int len = strlen(_str) + 1;
        ret = ltg_malloc((void **)&str, len);
        if (ret)
                GOTO(err_ret, ret);

        strcpy(str, _str);

        __errno__[idx] = str;
        __errno_idx__++;

        return 0;
err_ret:
        return ret;
}

const char *ltg_strerror(int errno)
{
        if (errno < ERRNO_KEEP_SYSEM) {
                return strerror(errno);
        } else {
                int idx = errno - ERRNO_KEEP_SYSEM;
                LTG_ASSERT(__errno__[idx]);
                return __errno__[idx];
        }
}
