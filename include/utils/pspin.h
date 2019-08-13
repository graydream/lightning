#ifndef __PSPIN_H__
#define __PSPIN_H__

#define PSPIN_DEBUG 0

typedef struct {
        char locked;
#if PSPIN_DEBUG
        uint32_t coreid;
#endif
} pspin_t;

int pspin_init(pspin_t *pspin);
int pspin_destroy(pspin_t *pspin);
int pspin_lock(pspin_t *pspin);
int pspin_trylock(pspin_t *pspin);
int pspin_locked(pspin_t *pspin);
int pspin_unlock(pspin_t *pspin);

#endif
