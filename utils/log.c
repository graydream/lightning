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

int log_init2(logmode_t logmode, const char *file, int log_max_mbytes)
{
        int ret, fd;
        struct stat stat;
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

                        ret = ltg_spin_init(&log->spin);
                        if (unlikely(ret)) {
                                fprintf(stderr, "log init %u", ret);
                                goto err_ret;
                        }

                        log->time = 0;
                        log->file_size = 0;

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

                        if (log_max_mbytes > 0) {
                                ret = fstat(fd, &stat);
                                if (ret == -1) {
                                        ret = errno;
                                        goto err_ret;
                                }

                                log->file_size = stat.st_size;
                                log->log_max_bytes = log_max_mbytes * 1024 * 1024;
                        } else {
                                log->log_max_bytes = 0;
                        }

                        strcpy(log->file, file);
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

int log_init(logmode_t logmode, const char *file)
{
        return log_init2(logmode, file, 0);
}

int log_destroy(void)
{
        if (__log__) {
                if (__log__->logmode == YLOG_FILE && __log__->logfd != -1)
                        (void) close(__log__->logfd);

                ltg_spin_destroy(&__log__->spin);

                ltg_free((void **)&__log__);
        }

        return 0;
}

static int __log_rollover(log_t *log)
{
        int fd, ret;
        char target_file[MAX_PATH_LEN], t[200];
        time_t now;
        struct tm *tm_now, tmt;

        close(log->logfd);

        time(&now);
        tm_now = localtime_r(&now, &tmt);
        strftime(t, 200, "%Y%m%d-%H%M%S", tm_now);
        snprintf(target_file, MAX_PATH_LEN, "%s-%s", log->file, t);

        ret = rename(log->file, target_file);
        if (ret == -1) {
                ret = errno;
                if (ret == ENOENT) {
                        // ret = 0;
                } else {
                        goto err_ret;
                }
        }

        fd = open(log->file, O_APPEND | O_CREAT | O_WRONLY, 0644);
        if (fd == -1) {
                ret = errno;
                goto err_ret;
        }

        log->logfd = fd;

        return 0;
err_ret:
        return ret;
}

static int __log_write_msg(log_t *log, const char *msg, int msglen)
{
        int ret;

        if (log->log_max_bytes) {
                ret = ltg_spin_lock(&log->spin);
                if (ret) {
                        fprintf(stderr, "ltg_spin_lock fail, ret %u", ret);
                        goto err_ret;
                }

                if (log->file_size >= log->log_max_bytes) {
                        ret = __log_rollover(log);
                        if (ret) {
                                goto err_ret;
                        }

                        log->file_size = 0;
                }
        }

        ret = write(log->logfd, msg, msglen);
        if (ret < 0) {
                ret = errno;
                goto err_ret;
        }

        if (log->log_max_bytes) {
                log->file_size += msglen;

                ltg_spin_unlock(&log->spin);
        }

        return 0;
err_ret:
        return ret;
}

int log_write(logtype_t type, const char *_msg)
{
        int ret, msglen = 0;

        (void) type;

        if (__log__ && __log__->logmode == YLOG_FILE && __log__->logfd != -1) {
                msglen = strlen(_msg);

                ret = __log_write_msg(__log__, _msg, msglen);
                if (unlikely(ret)) {
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
