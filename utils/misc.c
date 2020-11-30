#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <unistd.h>
#include <netdb.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/syscall.h>
#include <libgen.h>
#include <execinfo.h>
#include <poll.h>
#include <linux/fs.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <uuid/uuid.h>

#define DBG_SUBSYS S_LTG_UTILS

#include "ltg_utils.h"

int srv_running = 1;
int rdma_running = 1;

static uint64_t __global_hz__ = 0;

#undef MAXSIZE_LOGFILE
#define BACKTRACE_SIZE (1024 * 8)

int nid_cmp(const nid_t *keyid, const nid_t *dataid)
{
        int ret;

        if (keyid->id < dataid->id)
                ret = -1;
        else if (keyid->id > dataid->id)
                ret = 1;
        else {
                ret = 0;
        }

        return ret;
}

int coreid_cmp(const coreid_t *id1, const coreid_t *id2)
{
        int ret;
        
        ret = nid_cmp(&id1->nid, &id2->nid);
        if (ret)
                return ret;

        return id1->idx - id2->idx;
}

inline int _epoll_wait(int epfd, struct epoll_event *events,
                       int maxevents, int timeout)
{
        int ret, nfds;

        while (1) {
                nfds = epoll_wait(epfd, events, maxevents, timeout);
                if (nfds == -1) {
                        ret = errno;
                        if (ret == EINTR) {
                                DBUG("file recv loop interrupted by signal\n");
                                continue;
                        } else
                                GOTO(err_ret, ret);
                }

                break;
        }

        return nfds;
err_ret:
        return -ret;
}

inline ssize_t _read(int fd, void *buf, size_t count)
{
        int ret;

        while (1) {
                ret = read(fd, buf, count);
                if (ret == -1) {
                        ret = errno;
                        if (ret == EINTR)
                                continue;
                        else
                                GOTO(err_ret, ret);
                }

                break;
        }

        return ret;
err_ret:
        return -ret;
}

inline ssize_t _write(int fd, const void *buf, size_t count)
{
        int ret;

        while (1) {
                ret = write(fd, buf, count);
                if (ret == -1) {
                        ret = errno;

                        if (ret == EINTR)
                                continue;
                        else
                                GOTO(err_ret, ret);
                }

                break;
        }

        return ret;
err_ret:
        return -ret;
}

inline ssize_t _pread(int fd, void *buf, size_t count, off_t offset)
{
        int ret;

        while (1) {
                ret = pread(fd, buf, count, offset);
                if (ret == -1) {
                        ret = errno;

                        if (ret == EINTR)
                                continue;
                        else
                                GOTO(err_ret, ret);
                }

                break;
        }

        return ret;
err_ret:
        return -ret;
}

inline ssize_t _pwrite(int fd, const void *buf, size_t count, off_t offset)
{
        int ret;

        while (1) {
                ret = pwrite(fd, buf, count, offset);
                if (ret == -1) {
                        ret = errno;

                        if (ret == EINTR)
                                continue;
                        else
                                GOTO(err_ret, ret);
                }

                break;
        }

        return ret;
err_ret:
        return -ret;
}

inline ssize_t _recvmsg(int sockfd, struct msghdr *msg, int flags)
{
        int ret;

        while (1) {
                ret = recvmsg(sockfd, msg, flags);
                if (ret == -1) {
                        ret = errno;
                        if (ret == EINTR) {
                                DERROR("interrupted");
                                continue;
                        } else
                                goto err_ret;
                }

                break;
        }

        if (ret == 0) {
                ret = ECONNRESET;
                goto err_ret;
        }

        return ret;
err_ret:
        return -ret;
}

inline ssize_t _sendmsg(int sockfd, struct msghdr *msg, int flags)
{
        int ret;

        while (1) {
                ret = sendmsg(sockfd, msg, flags);
                if (ret == -1) {
                        ret = errno;
                        if (ret == EINTR) {
                                DERROR("interrupted");
                                continue;
                        } else
                                goto err_ret;
                }

                break;
        }

        return ret;
err_ret:
        return -ret;
}

inline int _sem_wait(sem_t *sem)
{
        int ret;

        while (1) {
                ret = sem_wait(sem);
                if (unlikely(ret)) {
                        ret = errno;
                        if (ret == EINTR)
                                continue;
                        else
                                GOTO(err_ret, ret);
                }

                break;
        }

        return 0;
err_ret:
        return ret;
}

inline int _sem_timedwait(sem_t *sem, const struct timespec *abs_timeout)
{
        int ret;

        while (1) {
                ret = sem_timedwait(sem, abs_timeout);
                if (unlikely(ret)) {
                        ret = errno;
                        if (ret == ETIMEDOUT)
                                goto err_ret;
                        else if (ret == EINTR)
                                continue;
                        else
                                GOTO(err_ret, ret);
                }

                break;
        }

        return 0;
err_ret:
        return ret;
}

inline int _sem_timedwait1(sem_t *sem, int tmo)
{
        int ret;
        struct timespec ts;

        LTG_ASSERT(tmo > 0);

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += tmo;

        while (1) {
                ret = sem_timedwait(sem, &ts);
                if (unlikely(ret)) {
                        ret = errno;
                        if (ret == ETIMEDOUT)
                                goto err_ret;
                        else if (ret == EINTR)
                                continue;
                        else
                                GOTO(err_ret, ret);
                }

                break;
        }

        return 0;
err_ret:
        return ret;
}

inline void init_global_hz(void)
{
        if (__global_hz__ == 0)
                __global_hz__ = cpu_freq_init();

        LTG_ASSERT(__global_hz__ != 0);
}

inline void _microsec_update_now(ltg_time_t *now)
{
#if SCHEDULE_TASKCTX_RUNTIME
        LTG_ASSERT(__global_hz__ != 0);
        now->tv = get_rdtsc();
#else
        _gettimeofday(&now->tv, NULL);
#endif
}

inline int64_t _microsec_time_used_from_now(ltg_time_t *prev)
{
#if SCHEDULE_TASKCTX_RUNTIME
        LTG_ASSERT(__global_hz__ != 0);
        return (get_rdtsc() - prev->tv) * 1000 * 1000 / __global_hz__;
#else
        struct timeval now;
        _gettimeofday(&now, NULL);

        return ((LLU)now.tv_sec - (LLU)prev->tv.tv_sec) * 1000 * 1000
                + (now.tv_usec - prev->tv.tv_usec);
#endif
}

inline int64_t _sec_time_used_from_now(ltg_time_t *prev)
{
#if SCHEDULE_TASKCTX_RUNTIME
        LTG_ASSERT(__global_hz__ != 0);
        return (get_rdtsc() - prev->tv) / __global_hz__;

#else
        return gettime() - prev->tv.tv_sec;
#endif
}

inline int64_t _microsec_time_used(ltg_time_t *t1, ltg_time_t *t2)
{
#if SCHEDULE_TASKCTX_RUNTIME
        LTG_ASSERT(__global_hz__ != 0);
        return (t2->tv - t1->tv) * 1000 * 1000 / __global_hz__;

#else
        return ((LLU)t2->tv.tv_sec - (LLU)t1->tv.tv_sec) * 1000 * 1000
                + (t2->tv.tv_usec - t1->tv.tv_usec);
#endif
}

inline int64_t _sec_time_used(ltg_time_t *t1, ltg_time_t *t2)
{
#if SCHEDULE_TASKCTX_RUNTIME
        LTG_ASSERT(__global_hz__ != 0);
        return (t2->tv - t1->tv) / __global_hz__;

#else
        return ((LLU)t2->tv.tv_sec - (LLU)t1->tv.tv_sec);
#endif
}

inline int64_t _time_used(const struct timeval *prev, const struct timeval *now)
{
        return ((LLU)now->tv_sec - (LLU)prev->tv_sec) * 1000 * 1000
                +  (now->tv_usec - prev->tv_usec);
}

int _set_value_off(const char *path, const char *value, int size, off_t offset, int flag)
{
        int ret, fd;

retry:
        fd = open(path, O_WRONLY | flag, 0644);
        if (fd < 0) {
                ret = errno;
                if ((flag & O_CREAT) && ret == ENOENT) {
                        ret = path_validate(path, 0, LLIB_DIRCREATE);
                        if (unlikely(ret))
                                GOTO(err_ret, ret);

                        goto retry;
                }

                if (ret == EEXIST)
                        goto err_ret;
                else
                        GOTO(err_ret, ret);
        }

        if (value) {
                ret = _pwrite(fd, value, size, offset);
                if (ret < 0) {
                        ret = -ret;
                        GOTO(err_ret, ret);
                }

                if (flag & O_SYNC)
                        fsync(fd);
        }

        close(fd);

        return 0;
err_ret:
        return ret;
}

int _set_value(const char *path, const char *value, int size, int flag)
{
        return _set_value_off(path, value, size, 0, flag);
}

int _get_value(const char *path, char *value, int buflen)
{
        int ret, fd;
        struct stat stbuf;

        ret = stat(path, &stbuf);
        if (unlikely(ret)) {
                ret = errno;
                if (ret == ENOENT)
                        goto err_ret;
                else
                        GOTO(err_ret, ret);
        }

        if (buflen < stbuf.st_size) {
                ret = EINVAL;
                GOTO(err_ret, ret);
        }

        fd = open(path, O_RDONLY);
        if (fd < 0) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        ret = _read(fd, value, buflen);
        if (ret < 0) {
                ret = -ret;
                GOTO(err_fd, ret);
        }

        close(fd);

        return ret;
err_fd:
        close(fd);
err_ret:
        return -ret;
}

int _dir_iterator(const char *path,
                  int (*callback)(const char *parent, const char *name, void *opaque),
                  void *opaque)
{
        int ret;
        DIR *dir;
        struct dirent debuf, *de;

        dir = opendir(path);
        if (dir == NULL) {
                ret = errno;
                if (ret == ENOENT)
                        goto err_ret;
                else {
                        DWARN("path %s\n", path);
                        GOTO(err_ret, ret);
                }
        }

        while (1) {
                ret = readdir_r(dir, &debuf, &de);
                if (ret < 0) {
                        ret = errno;
                        GOTO(err_dir, ret);
                }

                if (de == NULL)
                        break;

                if (strcmp(de->d_name, ".") == 0
                    || strcmp(de->d_name, "..") == 0)
                        continue;

                ret = callback(path, de->d_name, opaque);
                if (unlikely(ret))
                        GOTO(err_dir, ret);
        }

        closedir(dir);

        return 0;
err_dir:
        closedir(dir);
err_ret:
        return ret;
}

const char *_inet_ntoa(uint32_t addr)
{
        struct in_addr sin;

        sin.s_addr = addr;

        return inet_ntoa(sin);
}

int _inet_addr(struct sockaddr *sin, const char *host)
{
        int ret, herrno = 0;
        struct hostent  hostbuf, *result;
        char buf[MAX_BUF_LEN];

        if (AF_INET == sin->sa_family) {
                ret = gethostbyname_r(host, &hostbuf, buf, sizeof(buf),  &result, &herrno);
        } else if (AF_INET6 == sin->sa_family) {
                ret = gethostbyname2_r(host, AF_INET6, &hostbuf, buf, sizeof(buf),  &result, &herrno);
        } else {
                ret = EINVAL;
                GOTO(err_ret, ret);
        }
        if (unlikely(ret)) {
                ret = errno;
                if (ret == EALREADY || ret == EAGAIN) {
                        DERROR("connect addr %s\n", host);
                        ret = EAGAIN;
                        GOTO(err_ret, ret);
                } else
                        GOTO(err_ret, ret);
        }

        if (result) {
                if (AF_INET == sin->sa_family) {
                        memcpy(&((struct sockaddr_in *)sin)->sin_addr, result->h_addr, result->h_length);
                } else {
                        memcpy(&((struct sockaddr_in6 *)sin)->sin6_addr, result->h_addr, result->h_length);
                }
        } else {
                ret = ENONET;
                DWARN("connect addr %s ret (%u) %s\n", host, ret, strerror(ret));
                goto err_ret;
        }

        return 0;
err_ret:
        return ret;
}

const char *_inet_ntop(const struct sockaddr *addr)
{
        static char buf[INET6_ADDRSTRLEN];

        if (AF_INET == addr->sa_family) {
                return inet_ntop(AF_INET, &((struct sockaddr_in *)addr)->sin_addr, buf, sizeof(buf));
        } else if (AF_INET6 == addr->sa_family) {
                return inet_ntop(AF_INET6, &((struct sockaddr_in6 *)addr)->sin6_addr, buf, sizeof(buf));
        } else {
                return NULL;
        }
}

long int _random()
{
        struct timeval tv;

        gettimeofday(&tv, NULL);

        srandom(tv.tv_usec + getpid());

        return random();
}

void S_LTG *_opaque_encode(void *buf, uint32_t *len, ...)
{
        void *pos, *value;
        va_list ap;
        uint32_t valuelen, total;

        va_start(ap, len);

        pos = buf;
        total = 0;
        while (1) {
                value = va_arg(ap, char *);
                valuelen = va_arg(ap, uint32_t);

                //DBUG("encode %s len %u\n", (char *)value, valuelen);

                if (value == NULL)
                        break;

                memcpy(pos, &valuelen, sizeof(valuelen));
                pos += sizeof(valuelen);

                if (valuelen) {
                        memcpy(pos, value, valuelen);
                } else {
                }

                pos += valuelen;

                total += (sizeof(valuelen) + valuelen);
        }

        va_end(ap);

        *len = total;

        LTG_ASSERT(total <= MAX_MSG_SIZE);

        return buf + total;
}

const void S_LTG *_opaque_decode(const void *buf, uint32_t len, ...)
{
        const void *pos;
        const void **value;
        va_list ap;
        uint32_t *_valuelen, valuelen;

        va_start(ap, len);

        pos = buf;
        while (pos < buf + len) {
                value = va_arg(ap, const void **);
                _valuelen = va_arg(ap, uint32_t *);

                if (value == NULL)
                        break;

                memcpy(&valuelen, pos, sizeof(valuelen));
                pos += sizeof(valuelen);
                //memcpy(value, pos, valuelen);

                if (valuelen == 0) {
                        *value = NULL;
                } else {
                        *value = pos;
                }

                pos += valuelen;

                if (_valuelen)
                        *_valuelen = valuelen;

                //DBUG("decode %s len %u\n", (char *)value, valuelen);
        }

        va_end(ap);

        return pos;
}

int _set_text(const char *path, const char *value, int size, int flag)
{
        int ret, fd;
        char buf[MAX_BUF_LEN], path_tmp[MAX_BUF_LEN] = {}, _uuid[MAX_NAME_LEN];
        uuid_t uuid;

        (void) buf;
        
        uuid_generate(uuid);
        uuid_unparse(uuid, _uuid);

        snprintf(path_tmp, MAX_BUF_LEN, "%s.%s", path, _uuid);
retry:
        fd = open(path_tmp, O_WRONLY | flag, 0644);
        if (fd < 0) {
                ret = errno;
                if ((flag & O_CREAT) && ret == ENOENT) {
                        ret = path_validate(path_tmp, 0, LLIB_DIRCREATE);
                        if (unlikely(ret))
                                GOTO(err_ret, ret);

                        goto retry;
                }

                if (ret == EEXIST)
                        goto err_ret;
                else
                        GOTO(err_ret, ret);
        }

        LTG_ASSERT(size == (int)strlen(value));
        
        ret = _write(fd, value, strlen(value));
        if (ret < 0) {
                ret = -ret;
                GOTO(err_ret, ret);
        }

        ret = fsync(fd);
        if (ret < 0) {
                ret = errno;
                GOTO(err_fd, ret);
        }

        close(fd);

        /**
         * @todo 使用fnotify的情况下，会触发del事件
         */
        ret = rename(path_tmp, path);
        if (ret < 0) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        return 0;
err_fd:
        close(fd);
err_ret:
        return ret;
}

int _set_text_direct(const char *path, const char *value, int size, int flag)
{
        int ret, fd;
        char buf[MAX_BUF_LEN];

retry:
        fd = open(path, O_WRONLY | flag, 0644);
        if (fd < 0) {
                ret = errno;
                if ((flag & O_CREAT) && ret == ENOENT) {
                        ret = path_validate(path, 0, LLIB_DIRCREATE);
                        if (unlikely(ret))
                                GOTO(err_ret, ret);

                        goto retry;
                }

                if (ret == EEXIST)
                        goto err_ret;
                else
                        GOTO(err_ret, ret);
        }

        if (value) {
                if (value[size - 1] != '\n') {
                        strcpy(buf, value);
                        buf[size] = '\n';
                        ret = _write(fd, buf, size + 1);
                        if (ret < 0) {
                                ret = -ret;
                                GOTO(err_ret, ret);
                        }
                } else {
                        ret = _write(fd, value, size);
                        if (ret < 0) {
                                ret = -ret;
                                GOTO(err_ret, ret);
                        }
                }
        }

        close(fd);

        return 0;
err_ret:
        return ret;
}

int _get_text(const char *path, char *value, int buflen)
{
        int ret, fd;
        struct stat stbuf;

        ret = stat(path, &stbuf);
        if (unlikely(ret)) {
                ret = errno;
                if (ret == ENOENT)
                        goto err_ret;
                else
                        GOTO(err_ret, ret);
        }

        if (buflen < stbuf.st_size) {
                ret = EINVAL;
                GOTO(err_ret, ret);
        }

        fd = open(path, O_RDONLY);
        if (ret < 0) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        ret = _read(fd, value, buflen);
        if (ret < 0) {
                ret = -ret;
                close(fd);
                GOTO(err_ret, ret);
        }

        close(fd);

        if (ret > 0 && value[ret] != '\0') {
                value[ret] = '\0';
        }

        return ret;
err_ret:
        return -ret;
}

int _errno_net(int ret)
{
        int i;
        static int errlist[] = {ENONET, ETIMEDOUT, ETIME, ECONNRESET,
                                EHOSTUNREACH, EPIPE, ECONNREFUSED,
                                EHOSTDOWN, EADDRNOTAVAIL};
        static int count = sizeof(errlist) / sizeof(int);

        for (i = 0; i < count; i++) {
                if (errlist[i] == ret)
                        return ENONET;
        }

        return ret;
}

#define MAX_BACKTRACE 40
void _backtrace_caller(const char *name, int start, int end)
{
        int ret;
        void* array[MAX_BACKTRACE] = {0};
        uint32_t size = 0;
        char **strframe = NULL;
        uint32_t i = 0;
        char *buf, tmp[MAX_MSG_SIZE], time_buf[MAX_MSG_SIZE];
        time_t now = gettime();
        struct tm t;
        unsigned long pid = (unsigned long)getpid();
        unsigned long tid = (unsigned long)__gettid();

        size = backtrace(array, MAX_BACKTRACE);
        strframe = (char **)backtrace_symbols(array, size);
        strftime(time_buf, MAX_MSG_SIZE, "%F %T", localtime_safe(&now, &t));

        ret = ltg_malloc((void **)&buf, BACKTRACE_SIZE);
        if (unlikely(ret))
                return;

        buf[0] = '\0';
        if (name)
                sprintf(tmp, "%s backtrace:\n", name);
        else
                sprintf(tmp, "backtrace:\n");

        if (strlen(buf) + strlen(tmp) < BACKTRACE_SIZE) {
                sprintf(buf + strlen(buf), "%s", tmp);
        }

        end = _min(end, (int)size);
        for (i = start; (int)i < end; i++) {
                sprintf(tmp, "%s/%ld %ld/%ld frame[%d]: %s\n",
                                time_buf, now, pid, tid, i, strframe[i]);
                if (strlen(buf) + strlen(tmp) > BACKTRACE_SIZE) {
                        break;
                }

                sprintf(buf + strlen(buf), "%s", tmp);
        }

        DINFO1(BACKTRACE_SIZE, "%s", buf);
        ltg_free((void *)&buf);

        if (strframe) {
                ltg_free1(strframe);
                strframe = NULL;
        }
}

void _backtrace(const char *name)
{
        /* skip _backtrace & _backtrace_caller, so begin 2 */
        _backtrace_caller(name, 2, MAX_BACKTRACE - 2);
}

void calltrace(char *buf, size_t buflen)
{
        void* array[MAX_BACKTRACE] = {0};
        uint32_t size = 0;
        char **strframe = NULL;
        int start = 1;
        int end = MAX_BACKTRACE - 1;
        char tmp[MAX_NAME_LEN], func[MAX_NAME_LEN];

        size = backtrace(array, MAX_BACKTRACE);
        strframe = (char **)backtrace_symbols(array, size);

        buf[0] = '\0';
        end = _min(end, (int)size - 2);
        for (int i = end - 1; (int)i >= start; i--) {
                int ret = sscanf(strframe[i], "%[^(](%[^+)]+", tmp, func);
                if (ret == 1) {
                        strcpy(func, "unknow");
                }
                
                if (strlen(buf) + strlen(func) > buflen) {
                        break;
                }

                if (strlen(buf) == 0) {
                        sprintf(buf + strlen(buf), "%s", func);
                } else {
                        sprintf(buf + strlen(buf), "|%s", func);
                }
        }

        if (strframe) {
                ltg_free1(strframe);
                strframe = NULL;
        }
}

int ltg_thread_create(thread_func fn, void *arg, const char *name)
{
        int ret;
        pthread_t th;
        pthread_attr_t ta;

        (void) pthread_attr_init(&ta);
        (void) pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);
        
        ret = pthread_create(&th, &ta, fn, arg);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        char tname[MAX_NAME_LEN];
        snprintf(tname, MAX_BUF_LEN, "%s_%s", ltgconf_global.service_name, name);
        (void) pthread_setname_np(th, tname);
        
        DINFO("thread %s started\n", name);

        return 0;
err_ret:
        return ret;
}

static int __path_build(char *dpath)
{
        int ret;
        char *sla;

        sla = strchr(dpath + 1, '/');

        while (sla) {
                *sla = '\0';

                ret = access(dpath, F_OK);
                if (ret == -1) {
                        ret = errno;
                        if (ret == ENOENT) {
                                ret = mkdir(dpath, 0755);
                                if (ret == -1) {
                                        ret = errno;
                                        if (ret != EEXIST) {
                                                DERROR("mkdir(%s, ...) ret (%d) %s\n",
                                                       dpath, ret, strerror(ret));
                                                GOTO(err_ret, ret);
                                        }
                                }
                        } else
                                GOTO(err_ret, ret);
                }

                *sla = '/';
                sla = strchr(sla + 1, '/');
        }

        ret = access(dpath, F_OK);
        if (ret == -1) {
                ret = errno;
                if (ret == ENOENT) {
                        ret = mkdir(dpath, 0755);
                        if (ret == -1) {
                                ret = errno;
                                goto err_ret;
                        }
                } else
                        GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

int path_validate(const char *path, int isdir, int dircreate)
{
        int ret, len, i;
        char dpath[MAX_PATH_LEN];
        char *end, *sla, *str;

        if (path == NULL
            || (path[0] != '/' /* [1] */)) {
                ret = EFAULT;
                GOTO(err_ret, ret);
        }

        len = strlen(path) + 1;
        end = (char *)path + len;    /* *end == '\0' */

        if (!isdir && path[len - 2] == '/') {    /* "/file_name/" */
                ret = EINVAL;
                GOTO(err_ret, ret);
        }

        sla = (char *)path;
        while (sla < end && sla[1] == '/')   /* remove leading redundant '/' */
                sla++;

        if (sla == end)         /* all '/', -> "/////" */
                goto out;

        /* non-NULL for above [1] */
        str = strrchr(path, '/');
        while (str > sla && str[-1] == '/')
                str--;

        if (sla == str && !isdir)       /* "/file_name" */
                goto out;

        if (isdir) {    /* *end == '\0' */
                ;
        } else {        /* *end == '/' */
                end = str;
        }

        i = 0;
        while (1) {
                if (i == MAX_PATH_LEN) {
                        ret = ENAMETOOLONG;
                        GOTO(err_ret, ret);
                }

                while (sla < end && sla[0] == '/' && sla[1] == '/')
                        sla++;
                if (sla == end)
                        break;

                dpath[i] = *sla;
                i++;
                sla++;
        }
        dpath[i] = '\0';

        ret = access((const char *)dpath, F_OK);
        if (ret == -1) {
                ret = errno;

                if (ret == ENOENT && dircreate)
                        ret = __path_build(dpath);

                if (ret) {
                        DBUG("validate(%s(%s), %s dir, %s create) (%d) %s\n",
                               path, dpath, isdir ? "is" : "not",
                               dircreate ? "" : "no", ret, strerror(ret));

                        if (ret != EEXIST) {
                                DWARN("path %s\n", path);
                                GOTO(err_ret, ret);
                        }
                }
        }

out:
        return 0;
err_ret:
        return ret;
}

int timerange_create(timerange_t **range, const char *name, int64_t interval)
{
        int ret;
        timerange_t *_range;

        ret = ltg_malloc((void **)&_range, sizeof(timerange_t));
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        timerange_init(_range, name, interval);

        *range = _range;
        return 0;
}

int timerange_destroy(timerange_t **range)
{
        ltg_free((void **)range);
        return 0;
}

int timerange_init(timerange_t *range, const char *name, int64_t interval)
{
        strcpy(range->name, name);
        range->interval = interval;
        range->speed = 0;

        _gettimeofday(&range->p1.t, NULL);
        range->p1.count1 = 0;

        _gettimeofday(&range->p2.t, NULL);
        range->p2.count1 = 0;

        ltg_spin_init(&range->spin);

        return 0;
}

int timerange_update(timerange_t *range, uint64_t count1, timerange_func func, void *context)
{
        int ret;
        _gettimeofday(&range->p2.t, NULL);
        range->p2.count1 += count1;

        ret = ltg_spin_lock(&range->spin);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        int64_t interval = _time_used(&range->p1.t, &range->p2.t);
        if (interval >= range->interval) {
                LTG_ASSERT(range->p2.count1 >= range->p1.count1);

                range->speed = (range->p2.count1 - range->p1.count1) * 1000 * 1000 / interval;

                DBUG("name %s[%p] interval %jd speed %4ju p1 %ju p2 %ju\n",
                      range->name,
                      range,
                      interval,
                      range->speed,
                      range->p1.count1,
                      range->p2.count1);

                if (func) {
                        func(range, interval, context);
                }

                range->p1 = range->p2;

                ltg_spin_unlock(&range->spin);
                return 1;
        }

        ltg_spin_unlock(&range->spin);
        return 0;
}
