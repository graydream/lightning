#ifndef __LTG_MISC_H__
#define __LTG_MISC_H__

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <semaphore.h>
#include <netinet/in.h>

#include "ltg_list.h"
#include "ltg_id.h"
#include "lock.h"
#include "macros.h"

extern int _epoll_wait(int epfd, struct epoll_event *events,
                       int maxevents, int timeout);

extern ssize_t _read(int fd, void *buf, size_t count);
extern ssize_t _write(int fd, const void *buf, size_t count);

extern ssize_t _pread(int fd, void *buf, size_t count, off_t offset);
extern ssize_t _pwrite(int fd, const void *buf, size_t count, off_t offset);

extern ssize_t _recvmsg(int sockfd, struct msghdr *msg, int flags);
extern ssize_t _sendmsg(int sockfd, struct msghdr *msg, int flags);

extern int _sem_wait(sem_t *sem);
extern int _sem_timedwait(sem_t *sem, const struct timespec *abs_timeout);
extern int _sem_timedwait1(sem_t *sem, int tmo);

int _set_value(const char *path, const char *value, int size, int flag);
int _set_value_off(const char *path, const char *value, int size, off_t offset, int flag);
int _get_value(const char *path, char *value, int buflen);
int _set_text(const char *path, const char *value, int size, int flag);
int _set_text_direct(const char *path, const char *value, int size, int flag);
int _get_text(const char *path, char *value, int buflen);

const char *_inet_ntoa(uint32_t addr);
int _inet_addr(struct sockaddr *sin, const char *host);
const char *_inet_ntop(const struct sockaddr *addr);

long int _random(void);

void *_opaque_encode(void *buf, uint32_t *len, ...);
const void *_opaque_decode(const void *buf, uint32_t len, ...);

void _str_split(char *from, char split, char *to[], int *_count);

int _errno(int ret);
int _errno_net(int ret);

void _backtrace(const char *name);
void _backtrace1(const char *name, int start, int count);

int _gettimeofday(struct timeval *tv, struct timezone *tz);
int64_t _time_used(const struct timeval *prev, const struct timeval *now);

int eventfd_poll(int fd, int tmo, uint64_t *event);

int _dir_iterator(const char *path,
                  int (*callback)(const char *parent, const char *name, void *opaque),
                  void *opaque);

int ltg_thread_create(thread_func fn, void *arg, const char *name);
void calltrace(char *buf, size_t buflen);

// }}


struct tm *localtime_safe(time_t *_time, struct tm *tm_time);

int gettime_init();
time_t gettime();
void gettime_refresh(void *ctx);
int gettime_private_init();

void test_device();

uint64_t frandom();
int frandom_private_init();
int frandom_init();

/* cmp.c */
extern int nid_cmp(const nid_t *key, const nid_t *data);
extern int coreid_cmp(const coreid_t *id1, const coreid_t *id2);

/* crc32.c */
#define crc32_init(crc) ((crc) = ~0U)
extern int crc32_stream(uint32_t *_crc, const char *buf, uint32_t len);
extern uint32_t crc32_stream_finish(uint32_t crc);
int crc32_md_verify(const void *ptr, uint32_t len);
void crc32_md(void *ptr, uint32_t len);
uint32_t crc32_sum(const void *ptr, uint32_t len);

/* daemon.c */
int daemon_update(const char *key, const char *value);
int daemon_lock(const char *key);
int daemonlize(int daemon, int maxcore, char *chr_path, int preservefd, int64_t _maxopenfile);
int daemon_pid(const char *path);

/* hash.c */
extern uint32_t hash_str(const char *str);
extern uint32_t hash_mem(const void *mem, int size);

/* lock.c */
extern int ltg_rwlock_init(ltg_rwlock_t *rwlock, const char *name);
extern int ltg_rwlock_destroy(ltg_rwlock_t *rwlock);
extern int ltg_rwlock_rdlock(ltg_rwlock_t *rwlock);
extern int ltg_rwlock_tryrdlock(ltg_rwlock_t *rwlock);
extern int ltg_rwlock_wrlock(ltg_rwlock_t *rwlock);
extern int ltg_rwlock_trywrlock(ltg_rwlock_t *rwlock);
extern void ltg_rwlock_unlock(ltg_rwlock_t *rwlock);

/* path.c */
#define LLIB_ISDIR 1
#define LLIB_NOTDIR 0

#define LLIB_DIRCREATE 1
#define LLIB_DIRNOCREATE 0

extern int path_validate(const char *path, int isdir, int dircreate);

#endif
