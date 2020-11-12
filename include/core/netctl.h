#ifndef __NETCTL__
#define __NETCTL__

int netctl_init(uint64_t mask);
int netctl();
int netctl_postwait(const char *name, const coreid_t *coreid, const void *request,
                    int reqlen, int replen, const ltgbuf_t *wbuf, ltgbuf_t *rbuf,
                    int msg_type, int msg_size, int group, int timeout);
int netctl_postwait1(const char *name, const coreid_t *coreid,
                     const void *request, int reqlen,  void *reply,
                     int *replen, int msg_type, int group, int timeout);

#endif
