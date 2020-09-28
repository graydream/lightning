#ifndef __LTG_ID_H__
#define __LTG_ID_H__

#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

#include "ltg_def.h"

extern int srv_running;

#pragma pack(8)

typedef struct {
#if SCHEDULE_TASKCTX_RUNTIME
        uint64_t tv;
#else
        struct timeval tv;
#endif
} ltg_time_t;

typedef struct __task_t {
        int16_t taskid;
        int16_t scheid;
        int32_t retval;
        uint32_t fingerprint;
        ltg_time_t tv;
} task_t;

#pragma pack(8)

typedef struct {
        uintptr_t remote_addr[MAX_SGE];
        uint32_t  rkey;
        uint32_t  size;
} data_prop_t;

typedef struct {
        uint32_t idx;
        uint32_t figerprint;
        uint16_t tabid;
        data_prop_t data_prop;
} msgid_t;

#pragma pack()

#define MSGID_DUMP(msgid) do { \
        DBUG("msg (%d %x) tabid %d data %d rkey %u addr %p\n", \
             (msgid)->idx,                                        \
             (msgid)->figerprint,                                 \
             (msgid)->tabid,                                      \
             (msgid)->data_prop.size,                             \
             (msgid)->data_prop.rkey,                             \
             (msgid)->data_prop.remote_addr[0]);                  \
} while (0)


#define NID_FORMAT "%llu"
#define NID_ARG(_id) (LLU)(_id)->id

#pragma pack(8)

typedef struct {
        uint16_t id;
} nid_t;

typedef nid_t diskid_t;

typedef struct {
        nid_t nid;
        uint16_t idx;
} coreid_t;

#pragma pack()

static inline void str2nid(nid_t *nid, const char *str)
{
        sscanf(str, "%hu", &nid->id);
}

static inline void str2diskid(diskid_t *diskid, const char *str)
{
        sscanf(str, "%hu", &diskid->id);
}

static inline void nid2str(char *str, const nid_t *nid)
{
        snprintf(str, MAX_NAME_LEN, "%u", nid->id);
}

#define COREID_FORMAT "%s/%d"
#define COREID_ARG(__coreid__) netable_rname(&(__coreid__)->nid), (__coreid__)->idx

#endif
