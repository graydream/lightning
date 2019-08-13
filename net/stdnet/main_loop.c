#include <limits.h>
#include <time.h>
#include <string.h>
#include <sys/epoll.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_net.h"
#include "ltg_utils.h"
#include "ltg_net.h"
#include "ltg_core.h"

extern int ltg_nofile_max;

typedef struct {
        int epoll_rfd;   // 只监听EPOLL_IN事件

        int eventfd;     // sche->eventfd

        int idx;
        int busy;
        sem_t sem;

        sche_t *sche;
} worker_t;

#define EPOLL_TMO 30

static int __worker_count__;
static worker_t *__worker__;
//extern int ltg_nofile_max;
//static int __config_hz__ = 0;
static int __main_loop_request__ = 0;
int __main_loop_hold__ = 1;

int main_loop_check()
{
        return __main_loop_hold__ ? EAGAIN : 0;
}

static int __main_loop_worker_init(worker_t *worker)
{
        int ret, interrupt_eventfd;
        event_t ev;
        char name[MAX_NAME_LEN];

        snprintf(name, sizeof(name), "main_loop");
        ret = sche_create(&interrupt_eventfd, name, &worker->idx, &worker->sche, NULL);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        worker->eventfd = interrupt_eventfd;

        ev.data.fd = worker->eventfd;
        ev.events = EPOLLIN;
        ret = epoll_ctl(worker->epoll_rfd, EPOLL_CTL_ADD, worker->eventfd, &ev);
        if (ret < 0) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

/**
 * 读写分流
 * - 此处为读事件
 * - EPOLL_OUT在sdevent.c里处理
 *
 * @param _args
 * @return
 */
STATIC void *__main_loop_worker(void *_args)
{
        int ret, nfds;
        event_t ev;
        worker_t *worker;
        char buf[MAX_BUF_LEN];

        worker = _args;
        ret = __main_loop_worker_init(worker);
        if (unlikely(ret)) {
                DERROR("main loop init fail, ret %u\n", ret);
                EXIT(ret);
        }

        sem_post(&worker->sem);

        main_loop_hold();        

        while (1) {
                DBUG("running thread %u, epoll_fd %u, eventfd %u\n",
                      worker->idx, worker->epoll_rfd, worker->eventfd);

                nfds = _epoll_wait(worker->epoll_rfd, &ev,  1, EPOLL_TMO * 1000);
                if (nfds < 0) {
                        ret = errno;
                        LTG_ASSERT(0);
                }

                if (nfds == 0) {
                        //sche_scan(NULL);
                        continue;
                }

                sche_backtrace();

                DBUG("thread %u got new event, fd %u, epollfd %u\n",
                      worker->idx, ev.data.fd, worker->epoll_rfd);

                LTG_ASSERT(nfds == 1);
                LTG_ASSERT((ev.events & EPOLLOUT) == 0);

                if (ev.data.fd == worker->eventfd) {
                        DBUG("got sche event\n");

                        ret = read(ev.data.fd, buf, MAX_BUF_LEN);
                        if (ret < 0) {
                                ret = errno;
                                UNIMPLEMENTED(__DUMP__);
                        }

                        DBUG("read %u\n", ret);
                } else {
                        if ((ev.events & EPOLLRDHUP) || (ev.events & EPOLLERR)
                            || (ev.events & EPOLLHUP))  {
                                //__main_loop_remove_event(worker->epoll_rfd, ev.data.fd);
                                sdevent_exit(ev.data.fd);
                        } else {
                                ret = sdevent_recv(ev.data.fd);
                                if (ret == ECONNRESET) {
                                        //__main_loop_remove_event(worker->epoll_rfd, ev.data.fd);
                                        sdevent_exit(ev.data.fd);
                                }
                        }
                }

                worker->busy = 1;

                sche_run(NULL);
                //sche_scan(NULL);

#if 0
                if (ltgconf.daemon) {
                        aio_submit();
                }
#endif

                worker->busy = 0;
        }

        return NULL;
}

int main_loop_event(int sd, int event, int op)
{
        int ret;
        event_t ev;

        ev.events = event;
        ev.data.fd = sd;

        LTG_ASSERT(__worker_count__);

        ret = epoll_ctl(__worker__[sd % __worker_count__].epoll_rfd, op, sd, &ev);
        if (ret == -1) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

int main_loop_request(void (*exec)(void *buf), void *buf, const char *name)
{
        int ret, rand;
        sche_t *sche;

        if (__worker__ == NULL || __worker_count__ == 0) {
                ret = EAGAIN;
                GOTO(err_ret, ret);
        }

        rand = ++__main_loop_request__ % (__worker_count__);

        sche = __worker__[rand].sche;
        if (sche == NULL) {
                ret = EAGAIN;
                GOTO(err_ret, ret);
        }

        ret = sche_request(sche, -1, exec, buf, name);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

STATIC int __main_loop_worker_create(int threads)
{
        int ret, i;
        worker_t *_worker, *worker;

        ret = ltg_malloc((void **)&_worker, sizeof(worker_t) * threads);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        for (i = 0; i < threads; i++) {
                worker = &_worker[i];
                worker->idx = i;
                worker->busy = 0;

                worker->epoll_rfd = epoll_create(ltg_nofile_max);
                if (worker->epoll_rfd == -1) {
                        ret = errno;
                        GOTO(err_free, ret);
                }

                ret = sem_init(&worker->sem, 0, 0);
                if (unlikely(ret))
                        GOTO(err_free, ret);

                ret = ltg_thread_create(__main_loop_worker, worker, "main_loop");
                if (ret == -1) {
                        ret = errno;
                        GOTO(err_free, ret);
                }

                sem_wait(&worker->sem);
        }

        __worker_count__ = threads;
        __worker__ = _worker;

        return 0;
err_free:
        ltg_free((void **)&_worker);
err_ret:
        return ret;
}

int main_loop_create(int threads)
{
        int ret;

        __main_loop_hold__ = 1;
        
        DINFO("main loop create start, threads %u\n", threads);

        ret = __main_loop_worker_create(threads);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        DINFO("main loop inited, threads %d\n", threads);

        return 0;
err_ret:
        return ret;
}

void main_loop_start()
{
        DINFO("main loop start\n");
        __main_loop_hold__ = 0;
}
