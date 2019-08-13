#ifndef __HASH_TABLE_H__
#define __HASH_TABLE_H__

/* shamlessly stolean from http://www.sf.net/projects/sandiaportals/ */

#include <stdint.h>

#include "ltg_conf.h"

typedef struct hashtable_entry {
        uint32_t key;
        void *value;
        struct hashtable_entry *next;
} *htab_entry_t;

typedef struct hashtable {
        char name[MAX_NAME_LEN];
        unsigned int size;
        unsigned int num_of_entries;
        int (*compare_func)(const void *, const void *);
        uint32_t (*key_func)(const void *);
        htab_entry_t *entries;
} *htab_t;

/* hash_table.c */
extern htab_t htab_create(int (*compare_func)(const void *, const void *),
                                     uint32_t (*key_func)(const void *), const char *name);
extern void *htab_find(htab_t, void *comparator);
extern int htab_insert(htab_t, void *value, void *comparator,
                       int overwrite);
extern int htab_remove(htab_t, void *comparator, void **value);
extern void htab_iterate(htab_t, void (*handler)(void *, void *), void *arg);
extern void htab_filter(htab_t, int (*handler)(void *, void *),
                        void *arg, void (*thunk)(void *));
extern void htab_destroy(htab_t, void (*thunk)(void *, void *arg), void *arg);
int htab_resize(htab_t t, int size);
int htab_size(htab_t t);

#endif
