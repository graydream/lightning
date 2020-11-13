#ifndef __NETCTL__
#define __NETCTL__

int netctl_init(uint64_t mask);
int netctl_get(const coreid_t *coreid, coreid_t *netctl);
int netctl();

#endif
