#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_UTILS

#include "ltg_utils.h"

static log_t *__log__;

int log_init(logmode_t logmode, const char *file)
{
        int ret, fd;
        log_t *log;

        if (__log__ == NULL) {
                fd = -1;

                ret = ltg_malloc((void **)&log, sizeof(*log));
                if (ret)
                        GOTO(err_ret, ret);
                
                if (logmode == YLOG_FILE && file) {
                        ret = ltg_rwlock_init(&log->lock, NULL);
                        if (ret) {
                                fprintf(stderr, "log init %u", ret);
                                goto err_ret;
                        }

                        log->count = 0;
                        log->time = 0;

                        ret = path_validate(file, LLIB_NOTDIR, LLIB_DIRCREATE);
                        if (ret)
                                GOTO(err_ret, ret);
                        
                        fd = open(file, O_APPEND | O_CREAT | O_WRONLY, 0644);
                        if (fd == -1) {
                                ret = errno;
                                fprintf(stderr, "open(%s, ...) ret (%d) %s\n", file, ret,
                                       strerror(ret));
                                goto err_ret;
                        }
                } else if (logmode == YLOG_STDERR) {
                        fd = 2;
                }

                log->logfd = fd;
                log->logmode = logmode;
                __log__ = log;
        }

        return 0;
err_ret:
        return ret;
}

int log_destroy(void)
{
        if (__log__) {
                if (__log__->logmode == YLOG_FILE && __log__->logfd != -1)
                        (void) close(__log__->logfd);

                ltg_free((void **)&__log__);
        }

        return 0;
}

int log_write(logtype_t type, const char *_msg)
{
        int ret;

        (void) type;
        
        if (__log__ && __log__->logmode == YLOG_FILE && __log__->logfd != -1) {
                ret = write(__log__->logfd, _msg, strlen(_msg));
                if (ret < 0) {
                        ret = errno;
                        fprintf(stderr, "log write error %u", ret);
                        EXIT(ret);
                }
        } else
                fprintf(stderr, "%s", _msg);

        return 0;
}

int log_getfd()
{
        if (__log__)
                return __log__->logfd;
        else
                return -1;
}
