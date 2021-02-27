#ifndef __CPUSET_H__
#define __CPUSET_H__

#define MAX_CPU_COUNT 256

typedef struct {
        int cpu_id;
        int core_id;
        int node_id;             ///< NUMA node id
        int physical_package_id; ///< socket
        int used;
        int lockfd;
} coreinfo_t;


int cpuset_init(uint64_t mask);
int cpuset_set(const char *name, int cpu);
int cpuset_lock(int idx, coreinfo_t **_coreinfo, int shared);

#endif

