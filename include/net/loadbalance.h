#ifndef __LOADBALANCE_H__
#define __LOADBALANCE_H__

void loadbalance_update(const coreid_t *coreid, uint64_t _latency);
int loadbalance_get(const coreid_t *array, int count);
int loadbalance_init();


#endif
