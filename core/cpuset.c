#include <limits.h>
#include <time.h>
#include <string.h>
#include <sys/epoll.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/types.h>
#include <dirent.h>

#define DBG_SUBSYS S_LTG_CORE

#include "ltg_net.h"
#include "ltg_utils.h"
#include "ltg_rpc.h"
#include "ltg_core.h"

#define __CPU_PATH__ "/sys/devices/system/cpu"
#define __CPUSET_INIT__       1
#define __CPUSET_UNINIT__     0
#define MAX_NUMA_NODE 32

static coreinfo_t *__coreinfo__;

typedef struct {
        int threading_max;
        int polling_core;
        int aio_core;
        int hyper_threading;
} cpuinfo_t;

static cpuinfo_t cpuinfo = {0, 0, 0, 0};
static int __cpuset_init__ = __CPUSET_UNINIT__;

static int __get_socket_id(int cpu_id, int *socket_id)
{
        char path[128], *nodestr;
        int ret;
        DIR *dir;
        struct dirent debuf, *de;

        snprintf(path, 128, "%s%d", "/sys/devices/system/cpu/cpu", cpu_id);
        dir = opendir(path);
        if(dir == NULL){
                ret = errno;
                GOTO(err_ret, ret);
        }

        *socket_id = -1;

        while(1) {
                ret = readdir_r(dir, &debuf, &de);
                if (ret < 0){
                        ret = errno;
                        GOTO(err_close, ret);
                }

                if (de == NULL){
                        break;
                }

                nodestr = strstr(de->d_name, "node");
                if (nodestr != NULL) {
                        if(strlen(nodestr) != 5){
                                continue;
                        }

                        nodestr += 4;

                        *socket_id = atoi(nodestr);
                        break;
                }
        }

        closedir(dir);

        if(*socket_id == -1) {
                //DWARN("get numa information failed, switch to compatibility mode.\r\n");
                *socket_id = 0;
        }

        return 0;
err_close:
        closedir(dir);
err_ret:
        return ret;
}


static int __cpuset_getmax(const char *parent, const char *name, void *_max)
{
        int ret, idx, *max;

        (void) parent;
        max = _max;

        ret = sscanf(name, "cpu%d", &idx);
        if (ret != 1) {
                //DINFO("skip %s\n", name);
        } else {
                //DINFO("get cpu %u\n", idx);
                *max = *max < idx ? idx : *max;
        }

        return 0;
}

int __next_node_id__ = 0;
int __cpu_node_count__ = 0;

static int __cpu_lock(int cpu_id, int *_fd)
{
        int ret, fd, flags;
        char path[MAX_PATH_LEN];

        snprintf(path, MAX_PATH_LEN, "%s/cpulock/%d", ltgconf_global.workdir, cpu_id);

        DBUG("try lock cpu %s\n", path);
        ret = path_validate(path, LLIB_NOTDIR, LLIB_DIRCREATE);
        if (ret)
                GOTO(err_ret, ret);

        fd = open(path, O_CREAT | O_RDONLY, 0640);
        if (fd == -1) {
                ret = errno;
                GOTO(err_ret, ret);
        }

        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 ) {
                ret = errno;
                GOTO(err_fd, ret);
        }

        ret = fcntl(fd, F_SETFL, flags | FD_CLOEXEC);
        if (ret < 0) {
                ret = errno;
                GOTO(err_fd, ret);
        }

        ret = flock(fd, LOCK_EX | LOCK_NB);
        if (ret == -1) {
                ret = errno;
                if (ret == EWOULDBLOCK) {
                        DINFO("lock %s fail\n", path);
                        goto err_fd;
                } else
                        GOTO(err_fd, ret);
        }

        DINFO("lock cpu[%u] success\n", cpu_id);

        *_fd = fd;

        return 0;
err_fd:
        close(fd);
err_ret:
        return ret;
}

int cpuset_init(uint64_t mask)
{
        int i, ret, max = 0, count;
        char buf[MAX_BUF_LEN], path[MAX_PATH_LEN];
        coreinfo_t *coreinfo;
        int node_list[MAX_NUMA_NODE] = {0};
        int polling_core = CORE_MAX;

        if (__cpuset_init__ == __CPUSET_INIT__)
                return 0;

        ret = _dir_iterator(__CPU_PATH__, __cpuset_getmax, &max);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        count = max + 1;
        ret = ltg_malloc((void **)&__coreinfo__, sizeof(*__coreinfo__) * (count));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        memset(__coreinfo__, 0x0, sizeof(*__coreinfo__) * (count));

        for (i = 0; i < count; i++) {
                coreinfo = &__coreinfo__[i];
                coreinfo->cpu_id = i;

                snprintf(path, MAX_PATH_LEN, "%s/cpu%u/topology/core_id", __CPU_PATH__, i);
                ret = _get_text(path, buf, MAX_BUF_LEN);
                if (ret < 0) {
                        ret = -ret;
                        GOTO(err_ret, ret);
                }

                coreinfo->core_id = atoi(buf);

                snprintf(path, MAX_PATH_LEN, "%s/cpu%u/topology/physical_package_id", __CPU_PATH__, i);
                ret = _get_text(path, buf, MAX_BUF_LEN);
                if (ret < 0) {
                        if (ret == -ENOENT) 
                                break;

                        ret = -ret;
                        GOTO(err_ret, ret);
                }

                coreinfo->physical_package_id = atoi(buf);

                ret = __get_socket_id(i, &coreinfo->node_id);
                if (ret)
                        GOTO(err_ret, ret);

                node_list[coreinfo->node_id]++;

                DINFO("cpu[%u] node_id %u physical_package_id %u core_id %u\n",
                      i,
                      coreinfo->node_id,
                      coreinfo->physical_package_id,
                      coreinfo->core_id);
        }

        for (i = 0; i < MAX_NUMA_NODE; i++) {
               if (node_list[i])
                        __cpu_node_count__++;
               else
                       break;
        }

        cpuinfo.polling_core = polling_core;
        cpuinfo.aio_core = 0;

        if (cpuinfo.polling_core < 1) {
                DINFO("force set polling_core 1\n");
                cpuinfo.polling_core = 1;
        }

        cpuinfo.threading_max = count;

        if (count < core_count(mask)) {
                ret = EINVAL;
                DERROR("bad coremask config, need %u got %u\n", core_count(mask), count);
                GOTO(err_ret, ret);
        }
        
        DINFO("core count %u polling %u\n", count, cpuinfo.polling_core);

        __cpuset_init__ = __CPUSET_INIT__;

        return 0;
err_ret:
        return ret;
}

int cpuset_set(const char *name, int cpu)
{
        int ret;
        cpu_set_t cmask;
        size_t n;
        coreinfo_t *coreinfo;

        if (!ltgconf_global.daemon || cpu == -1)
                return 0;

        coreinfo = &__coreinfo__[cpu];
        DINFO("set %s @ cpu[%u], core[%u], thread[%u]\n", name,
              coreinfo->physical_package_id, coreinfo->core_id, cpu);

        n = sysconf(_SC_NPROCESSORS_ONLN);

        CPU_ZERO(&cmask);
        CPU_SET(cpu, &cmask);

        ret = sched_setaffinity(0, n, &cmask);
        if (unlikely(ret)) {
                ret = errno;
                DWARN("bad cpu set %u\n", cpu);
                GOTO(err_ret, ret);
        }

        CPU_ZERO(&cmask);

        return 0;
err_ret:
        return ret;
}

int cpuset_lock(int idx, coreinfo_t **_coreinfo)
{
        int ret, fd;
        coreinfo_t *coreinfo;

        coreinfo = &__coreinfo__[idx];

        ret = __cpu_lock(coreinfo->cpu_id, &fd);
        if (ret) {
                DWARN("cpu[%u] used by other process\n", idx);
                ret = EBUSY;
                GOTO(err_ret, ret);
        }

        coreinfo->lockfd = fd;
        *_coreinfo = coreinfo;

        DINFO("core %u/%u/%u/%u\n",
              coreinfo->cpu_id, coreinfo->node_id,
              coreinfo->physical_package_id,
              coreinfo->core_id);

        return 0;
err_ret:
        return ret;
}
