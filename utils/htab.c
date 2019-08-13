

#include <stdint.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_UTILS

#include "ltg_utils.h"

/*
 * a very simple hash table implementation with paramerterizable
 * comparison and key generation functions. it does resize
 * in order to accomidate more entries, and collapses
 * the table to free unused memory
 */

uint32_t key_from_str(char *s)
{
        return hash_str(s);
}

static htab_entry_t *htab_lookup(htab_t t, void *comparator, uint32_t k,
                                     int (*compare_func)(const void *, const void *),
                                     int *success)
{
        uint32_t key = k % t->size;
        htab_entry_t *i;

        for (i = &(t->entries[key]); *i; i = &((*i)->next)) {
                if (compare_func && ((*i)->key == k))
                        if ((*t->compare_func)((*i)->value, comparator) == 0) {
                                *success = 1;
                                return i;
                        }
        }

        *success = 0;

        return &(t->entries[key]);
}

int htab_resize(htab_t t, int size)
{
        int ret, old_size, i, success;
        uint32_t len;
        void *ptr;
        htab_entry_t *old_entries, j, n, *position;

        len = sizeof(htab_entry_t) * size;

        ret = ltg_malloc(&ptr, len);
        if (ret)
                GOTO(err_ret, ret);
        memset(ptr, 0, len);

        old_size = t->size;
        old_entries = t->entries;

        t->size = size;
        t->entries = (htab_entry_t *)ptr;

        for (i = 0; i < old_size; i++)
                for (j = old_entries[i]; j; j = n) {
                        n = j->next;
                        position = htab_lookup(t, 0, j->key, 0, &success);
                        j->next = *position;
                        *position = j;
                }

        ltg_free((void **)&old_entries);

        DINFO("resize %s, new size %u\n", t->name, size);

        return 0;
err_ret:
        return ret;
}

/* Function: htab_create
 * Arguments: compare_function: a function to compare
 *                              a table instance with a correlator
 *            key_function: a function to generate a 32 bit 
 *                          hash key from a correlator
 * Returns: a pointer to the new table or null
 */

#define INIT_HASH_TABLE_SIZE 4

htab_t htab_create(int (*compare_func)(const void *, const void *),
                              uint32_t (*key_func)(const void *), const char *name)
{
        int ret;
        uint32_t len;
        void *ptr;
        htab_t new;

        len = sizeof(struct htab);

        ret = ltg_malloc(&ptr, len);
        if (ret)
                GOTO(err_ret, ret);

        new = (htab_t)ptr;

        len = sizeof(htab_entry_t) * INIT_HASH_TABLE_SIZE;

        ret = ltg_malloc(&ptr, len);
        if (ret)
                GOTO(err_table, ret);
        memset(ptr, 0, len);

        new->size = INIT_HASH_TABLE_SIZE;
        new->num_of_entries = 0;
        new->entries = (htab_entry_t *)ptr;
        new->compare_func = compare_func;
        new->key_func = key_func;
        strncpy(new->name, name, MAX_NAME_LEN - 1);

        return new;
err_table:
        ltg_free((void **)&new);
err_ret:
        return NULL;
}

/* Function: htab_find
 * Arguments: t: a table to look in
 *            comparator: a value to access the table entry
 * Returns: the element references to by comparator, or null
 */
void *htab_find(htab_t t, void *comparator)
{
        int success;
        htab_entry_t *entry;

        entry = htab_lookup(t, comparator, (*t->key_func)(comparator),
                                 t->compare_func, &success);

        if (success)
                return (*entry)->value;

        return NULL;
}

/* Function: htab_insert
 * Arguments: t: a table to insert the object
 *            value: the object to put in the table
 *            comparator: the value by which the object 
 *                        will be addressed
 * Returns: 0 or errno
 */

#define HASH_TABLE_RESIZE_NUM 2
int htab_insert(htab_t t, void *value, void *comparator, int overwrite)
{
        int ret, success;
        uint32_t k, len;
        htab_entry_t *position, entry;
        void *ptr;

        k = (*t->key_func)(comparator);

        position = htab_lookup(t, comparator, k, t->compare_func, &success);

        if (success) {
                if (!overwrite) {
                        ret = EEXIST;
                        GOTO(err_ret, ret);
                }

                entry = *position;
        } else {
                len = sizeof(struct htab_entry);

                ret = ltg_malloc(&ptr, len);
                if (ret)
                        GOTO(err_ret, ret);

                entry = (htab_entry_t)ptr;

                entry->next = *position;
                *position = entry;

                t->num_of_entries++;
        }

        entry->value = value;
        entry->key = k;

        if (t->num_of_entries > t->size)
                (void) htab_resize(t, t->size * HASH_TABLE_RESIZE_NUM);

        return 0;
err_ret:
        return ret;
}

/* Function: htab_remove
 * Arguments: t: the table to remove the object from
 *            comparator: the index value of the object to remove
 * Returns: 0 or ENOENT
 */

int htab_remove(htab_t t, void *comparator, void **value)
{
        int success;
        htab_entry_t *position, entry;

        position = htab_lookup(t, comparator, (*t->key_func)(comparator),
                                t->compare_func, &success);

        if (!success)
                return ENOENT;

        entry = *position;
        *position = entry->next;

        if (value != NULL)
                *value = entry->value;

        ltg_free((void **)&entry);

        t->num_of_entries--;

#if 0
        if (t->num_of_entries < t->size / HASH_TABLE_RESIZE_NUM)
                (void) htab_resize(t, t->size / HASH_TABLE_RESIZE_NUM);
#endif

        return 0;
}

/* Function: htab_iterate
 * Arguments: t: the table to iterate over
 *            handler: a function to call with each element
 *                     of the table, along with arg
 *            arg: the opaque object to pass to handler
 * Returns: nothing
 */
void htab_iterate(htab_t t, void (*handler)(void *, void *),
                                void *arg)
{
        unsigned int i;
        htab_entry_t *j, *next;

        for (i = 0; i < t->size; i++) {
                for (j = t->entries + i; *j; j = next) {
                        next = &((*j)->next);

                        (*handler)(arg, (*j)->value);
                }
        }
}

/* Function: htab_filter
 * Arguments: t: the table to iterate over
 *            handler: a function to call with each element
 *                     of the table, along with arg
 *            arg: the opaque object to pass to handler
 * Returns: nothing
 * Notes: operations on the table inside handler are not safe
 *
 * filter_table_entires() calls the handler function for each
 *   item in the table, passing it and arg. The handler function
 *   returns 1 if it is to be retained in the table, and 0
 *   if it is to be removed.
 */
void htab_filter(htab_t t, int (*handler)(void *, void *),
                               void *arg, void (*thunk)(void *))
{
        unsigned int i;
        htab_entry_t *j, *next, entry;

        for (i = 0; i < t->size; i++)
                for (j = t->entries + i; *j; j = next) {
                        next = &((*j)->next);

                        if (!(*handler)(arg, (*j)->value)) {
                                next = j;

                                entry = *j;
                                *j = (*j)->next;

                                if (thunk)
                                        (*thunk)(entry->value);

                                ltg_free((void **)&entry);

                                t->num_of_entries--;

                                if (t->num_of_entries
                                    < t->size / HASH_TABLE_RESIZE_NUM)
                                        (void) htab_resize(t,
                                               t->size / HASH_TABLE_RESIZE_NUM);
                        }
                }
}

/* Function: destroy_table
 * Arguments: t: the table to free
 *            thunk: a function to call with each element,
 *                   most likely free()
 * Returns: nothing
 */
void htab_destroy(htab_t t, void (*thunk)(void *, void *arg), void *arg)
{
        unsigned int i;
        htab_entry_t entry, next;

        for (i = 0; i < t->size; i++)
                for (entry = t->entries[i]; entry; entry = next) {
                        next = entry->next;

                        if (thunk)
                                (*thunk)(entry->value, arg);
                        else
                                ltg_free((void **)&entry->value);

                        ltg_free((void **)&entry);
                }

        ltg_free((void **)&t->entries);
        ltg_free((void **)&t);
}
