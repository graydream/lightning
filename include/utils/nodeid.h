#ifndef __NODEID__
#define __NODEID__

#include <stdint.h>

typedef uint32_t nodeid_t; //instence id
#define NODEID_MAX (INT16_MAX)

//int nodeid_newid(nodeid_t *id, const char *name);
int nodeid_drop(nodeid_t id);
int nodeid_used(nodeid_t id);
int nodeid_init(nodeid_t *_id, const char *name);
int nodeid_load(nodeid_t *_id);
int nodeid_newid(nodeid_t *_id, const char *name);

#endif
