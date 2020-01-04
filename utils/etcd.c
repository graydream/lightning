#include <dirent.h>

#define DBG_SUBSYS S_LTG_UTILS

#include "ltg_net.h"
#include "3part.h"
#include "utils/ltg_conf.h"
#include "ltg_utils.h"
#include "ltg_core.h"

#define __ETCD_SRV__  "127.0.0.1:2379"

extern ltgconf_t ltgconf_global;

static int __etcd_open_str(char *server, etcd_session *_sess);
static int __etcd_get__(const char *srv, const char *key, etcd_node_t **result, int consistent);

static int  __etcd_set__(const char *key, const char *value,
                           const etcd_prevcond_t *precond, etcd_set_flag flag, unsigned int ttl)
{
        int ret;
        etcd_session sess;
        char *host;

        DBUG("write %s\n", key);

        host = strdup(__ETCD_SRV__);
        ret = __etcd_open_str(host, &sess);
        if (ret) {
                GOTO(err_ret, ret);
        }

        ret = etcd_set(sess, (void *)key, (void *)value, (void *)precond, flag,
                       ttl, ltgconf_global.rpc_timeout / 2);
        if (ret != ETCD_OK) {
                if (ret == ETCD_PREVCONT) {
                        ret = EEXIST;
                } else if (ret == ETCD_ENOENT) {
                        ret = ENOENT;
                } else {
                        ret = EAGAIN;
                }

                GOTO(err_close, ret);
        }
        
        etcd_close_str(sess);

        free(host);
        return 0;
err_close:
        etcd_close_str(sess);
err_ret:
        free(host);
        return ret;
}

static int __etcd_get__(const char *srv, const char *key, etcd_node_t **result, int consistent)
{
        int ret;
        etcd_session sess;
        etcd_node_t *node;
        char *host;

        DBUG("read %s\n", key);
        
        host = strdup(srv);
        ret = __etcd_open_str(host, &sess);
        if (ret) {
                GOTO(err_ret, ret);
        }
                
        ret = etcd_get(sess, (void *)key, ltgconf_global.rpc_timeout / 2, &node, consistent);
        if(ret != ETCD_OK){
                if (ret == ETCD_ENOENT) {
                        ret = ENOKEY;
                } else {
                        ret = EAGAIN;
                }

                GOTO(err_close, ret);
        }

        *result = node;

        etcd_close_str(sess);

        free(host);
        return 0;
err_close:
        etcd_close_str(sess);
err_ret:
        free(host);
        return ret;
}

static int __etcd_set_request(va_list ap)
{
        int ret;
        const char *key = va_arg(ap, const char *);
        const char *value = va_arg(ap, const char *);
        const etcd_prevcond_t *precond = va_arg(ap, const etcd_prevcond_t *);
        etcd_set_flag flag = va_arg(ap, etcd_set_flag);
        unsigned int ttl = va_arg(ap, unsigned int);

        va_end(ap);

        ret = __etcd_set__(key, value, precond, flag, ttl);
        if (ret) {
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

static int __etcd_set(const char *key, const char *value,
                      const etcd_prevcond_t *precond, etcd_set_flag flag, unsigned int ttl)
{
        int ret;

        ANALYSIS_BEGIN(0);

        if (likely(sche_running())) {
                ret = sche_newthread(SCHE_THREAD_ETCD, _random(), FALSE,
                                         "etcd_set", -1, __etcd_set_request,
                                         key, value, precond, flag, ttl);
                if (unlikely(ret)) {
                        //LTG_ASSERT(ret == ENOKEY);
                        GOTO(err_ret, ret);
                }
        } else {
                if (unlikely(sche_self()))
                        DERROR("etcd request in core but no task!!!\n");

                ret = __etcd_set__(key, value, precond, flag, ttl);
                if (ret) {
                        GOTO(err_ret, ret);
                }
        }

        ANALYSIS_END(0, IO_WARN, NULL);
        //ANALYSIS_ASSERT(0, 1000 * 1000 * (ltgconf_global.rpc_timeout), NULL);

        return 0;
err_ret:
        ANALYSIS_END(0, IO_WARN, NULL);
        //ANALYSIS_ASSERT(0, 1000 * 1000 * (ltgconf_global.rpc_timeout), NULL);
        return ret;
}

static int __etcd_get_request(va_list ap)
{
        int ret;
        const char *key = va_arg(ap, const char *);
        etcd_node_t **result = va_arg(ap, etcd_node_t **);
        int consistent = va_arg(ap, int);
        etcd_node_t *node = NULL;

        va_end(ap);

        /* ETCD_OK, it ok
         * ETCD_PROTOCAL_ERR, other http error
         * ETCD_ERR, curl failed, maybe connect error
         * ETCD_NOENT, no such key
         * ETCD_PREVCONT, key exist, but previous condition error, can be exist, or other*/
         
        ret = __etcd_get__(__ETCD_SRV__, key, &node, consistent);
        if (ret) {
                GOTO(err_ret, ret);
        }

        *result = node;

        return 0;
err_ret:
        return ret;
}

static int __etcd_get(const char *key, etcd_node_t **result, int consistent)
{
        int ret;
        etcd_node_t *node = NULL;

        ANALYSIS_BEGIN(0);

        if (likely(sche_running())) {
                ret = sche_newthread(SCHE_THREAD_ETCD, _random(), FALSE, "etcd_get", -1, __etcd_get_request,
                                key, result, consistent);
                if (unlikely(ret)) {
                        //LTG_ASSERT(ret == ENOKEY);
                        GOTO(err_ret, ret);
                }
        } else {
                if (unlikely(sche_self()))
                        DERROR("etcd request in core but no task!!!\n");

                ret = __etcd_get__(__ETCD_SRV__, key, &node, consistent);
                if (ret) {
                        GOTO(err_ret, ret);
                }

                *result = node;
        }

        ANALYSIS_END(0, IO_WARN, NULL);
        //ANALYSIS_ASSERT(0, 1000 * 1000 * (ltgconf_global.rpc_timeout), NULL);

        return 0;
err_ret:
        ANALYSIS_END(0, IO_WARN, NULL);
        //ANALYSIS_ASSERT(0, 1000 * 1000 * (ltgconf_global.rpc_timeout), NULL);
        return ret;
}

static int __etcd_delete__(etcd_session sess, char *key)
{
        int ret;
        
        ret = etcd_delete(sess, key);
        if (unlikely(ret != ETCD_OK)) {
                if (ret == ETCD_ENOENT) {
                        ret = ENOENT;
                } else {
                        ret = EAGAIN;
                }

                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

static int __etcd_del_request(va_list ap)
{
        int ret;
        etcd_session sess = va_arg(ap, etcd_session);
        char *key = va_arg(ap, char *);

        va_end(ap);

        DBUG("del %s\n", key);

        ret = __etcd_delete__(sess, key);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

static int __etcd_del(etcd_session sess, char *key)
{
        int ret;

        ANALYSIS_BEGIN(0);

        LTG_ASSERT(sess);
        if (likely(sche_running())) {
                ret = sche_newthread(SCHE_THREAD_ETCD, _random(), FALSE, "etcd_del", -1, __etcd_del_request,
                                sess, key);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }
        } else {
                if (unlikely(sche_self()))
                        DERROR("etcd request in core but no task!!!\n");

                ret = __etcd_delete__(sess, key);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }
        }

        ANALYSIS_END(0, IO_WARN, NULL);

        return 0;
err_ret:
        ANALYSIS_END(0, IO_WARN, NULL);
        return ret;
}

static int __etcd_deletedir__(etcd_session sess, char *key, int recursive)
{
        int ret;

        ret = etcd_deletedir(sess, key, recursive);
        if (unlikely(ret != ETCD_OK)) {
                if (ret == ETCD_ENOENT) {
                        ret = ENOENT;
                } else {
                        ret = EAGAIN;
                }

                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}


static int __etcd_del_dir_request(va_list ap)
{
        int ret;
        etcd_session sess = va_arg(ap, etcd_session);
        char *key = va_arg(ap, char *);
        int recursive = va_arg(ap, int);

        va_end(ap);

        DBUG("del dir %s, recursive: %d\n", key, recursive);

        ret = __etcd_deletedir__(sess, key, recursive);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

static int __etcd_del_dir(etcd_session sess, char *key, int recursive)
{
        int ret;

        ANALYSIS_BEGIN(0);

        LTG_ASSERT(sess);
        if (likely(sche_running())) {
                ret = sche_newthread(SCHE_THREAD_ETCD, _random(), FALSE, "etcd_del",
                                         -1, __etcd_del_dir_request,
                                         sess, key, recursive);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }
        } else {
                if (unlikely(sche_self()))
                        DERROR("etcd request in core but no task!!!\n");

                ret = __etcd_deletedir__(sess, key, recursive);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }
        }

        ANALYSIS_END(0, IO_WARN, NULL);

        return 0;
err_ret:
        ANALYSIS_END(0, IO_WARN, NULL);
        return ret;
}

static int __etcd_open_str__(va_list ap)
{
        int ret;
        char *server = va_arg(ap, char *);
        etcd_session  *result = va_arg(ap, etcd_session *);
        etcd_session  sess;

        va_end(ap);

        DBUG("open %s\n", server);

        sess = etcd_open_str(server);
        if(!sess){
                ret = ENOENT;
                GOTO(err_ret, ret);
        }

        *result = sess;

        return 0;
err_ret:
        return ret;
}

static int __etcd_open_str(char *server, etcd_session *_sess)
{
        int ret;
        etcd_session  sess;

        if (likely(sche_running())) {
                ret = sche_newthread(SCHE_THREAD_ETCD, _random(), FALSE, "etcd_open", -1, __etcd_open_str__,
                                server,  &sess);
                if (unlikely(ret)) {
                        //LTG_ASSERT(ret == ENOKEY);
                        GOTO(err_ret, ret);
                }
        } else {
                if (unlikely(sche_self()))
                        DERROR("etcd request in core but not with task!!!\n");

                sess = etcd_open_str(server);
                if(!sess){
                        ret = ENOENT;
                        GOTO(err_ret, ret);
                }
        }

        *_sess = sess;
        LTG_ASSERT(sess);

        return 0;
err_ret:
        return ret;
}

int etcd_mkdir(const char *prefix, const char *dir, int ttl)
{
        int ret;
        char key[MAX_PATH_LEN];
        etcd_prevcond_t precond;

        //LTG_ASSERT(sche_self() == 0);

        precond.type = prevExist;
        precond.value = "false";

        snprintf(key, MAX_NAME_LEN, "/%s/%s/%s", ltgconf_global.system_name, prefix, dir);
        ret = __etcd_set(key, NULL, &precond, ETCD_DIR, ttl);
        if (ret) {
                //DWARN("mkdir dir: %s, ret: %d\n", dir, ret);
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

int etcd_create_text(const char *prefix, const char *_key, const char *_value, int ttl)
{
        int ret;
        char key[MAX_PATH_LEN], value[MAX_PATH_LEN];
        etcd_prevcond_t precond;

        LTG_ASSERT(strcmp(_value, ""));

        snprintf(key, MAX_NAME_LEN, "/%s/%s/%s", ltgconf_global.system_name, prefix, _key);
        strcpy(value, _value);

        precond.type = prevExist;
        precond.value = "false";

        ret = __etcd_set(key, value, &precond, 0, ttl);
        if (ret) {
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

int etcd_create(const char *prefix, const char *_key, const void *_value,
                int valuelen, int ttl)
{
        int ret;
        char buf[MAX_BUF_LEN];
        size_t size;

        size = MAX_BUF_LEN;
        ret = urlsafe_b64_encode(_value, valuelen, buf, &size);
        LTG_ASSERT(ret == 0);

        ret = etcd_create_text(prefix, _key, buf, ttl);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int etcd_update_text(const char *prefix, const char *_key, const char *_value,
                     int *idx, int ttl)
{
        int ret;
        etcd_prevcond_t precond;
        char key[MAX_PATH_LEN], value[MAX_PATH_LEN], tmp[MAX_BUF_LEN];

        LTG_ASSERT(strcmp(_value, ""));

        snprintf(key, MAX_NAME_LEN, "/%s/%s/%s", ltgconf_global.system_name, prefix, _key);
        strcpy(value, _value);

        if (idx) {
                snprintf(tmp, MAX_NAME_LEN, "%d", *idx);
                precond.type = prevIndex;
                precond.value = tmp;
        } else {
                precond.type = prevExist;
                precond.value = "true";
        }

        ret = __etcd_set(key, value, &precond, 0, ttl);
        if (ret) {
                GOTO(err_ret, ret);
        }

#if 0
        if (idx) {
                ret = etcd_get_text(prefix, _key, tmp, idx);
                if (ret)
                        GOTO(err_ret, ret);

                LTG_ASSERT(strcmp(_value, tmp) == 0);
        }
#endif
        
        return 0;
err_ret:
        return ret;
}

int etcd_update(const char *prefix, const char *_key, const void *_value, int valuelen,
                int *idx, int ttl)
{
        int ret;
        char buf[MAX_BUF_LEN];
        size_t size;

        size = MAX_BUF_LEN;
        ret = urlsafe_b64_encode(_value, valuelen, buf, &size);
        LTG_ASSERT(ret == 0);

        ret = etcd_update_text(prefix, _key, buf, idx, ttl);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int etcd_get_text(const char *prefix, const char *_key, char *value, int *idx)
{
        int ret;
        char key[MAX_PATH_LEN];
        etcd_node_t *node = NULL;

        snprintf(key, MAX_NAME_LEN, "/%s/%s/%s", ltgconf_global.system_name, prefix, _key);
        ret = __etcd_get(key, &node, 1);
        if(ret){
                GOTO(err_ret, ret);
        }

        if (node->dir) {
                ret = EISDIR;
                GOTO(err_free, ret);
        }

        LTG_ASSERT(node->key && node->value);

        strcpy(value, node->value);

        if (idx)
                *idx = node->modifiedIndex;

        free_etcd_node(node);

        return 0;
err_free:
        free_etcd_node(node);
err_ret:
        return ret;
}

int etcd_get_bin(const char *prefix, const char *_key, void *_value,
                 int *_valuelen, int *idx)
{
        int ret;
        char buf[MAX_BUF_LEN];
        size_t size;

        ret = etcd_get_text(prefix, _key, buf, idx);
        if (ret)
                GOTO(err_ret, ret);

        size = MAX_BUF_LEN;
        ret = urlsafe_b64_decode(buf, strlen(buf), _value, &size);
        LTG_ASSERT(ret == 0);

        if (_valuelen) {
                LTG_ASSERT((int)size <= *_valuelen);
                *_valuelen = size;
        }

        return 0;
err_ret:
        return ret;
}

static void __etcd_readdir(const char *_key, const etcd_node_t *node,
                           char *buf, int *_buflen)
{
        int i, buflen, keylen, reclen;
        struct dirent *de;
        const char *key;

        keylen = strlen(_key) + 1;
        de = (void *)buf;
        buflen = *_buflen;
        for (i = 0; i < node->num_node; i++) {
                key = ((etcd_node_t*)node->nodes[i])->key + keylen;
                reclen = sizeof(*de) + strlen(key) + 1;

                if ((void *)de - (void *)buf + reclen > buflen)
                        break;

                strcpy(de->d_name, key);
                de->d_reclen = reclen;
                de->d_off = 0;

                DBUG("%s : (%s)\n", _key, de->d_name);

                de = (void *)de + de->d_reclen;
        }

        *_buflen = (void *)de - (void *)buf;
}

int etcd_readdir(const char *_key, char *buf, int *buflen)
{
        int ret;
        char key[MAX_PATH_LEN];
        etcd_node_t *node = NULL;

        snprintf(key, MAX_NAME_LEN, "/%s/%s", ltgconf_global.system_name, _key);
        ret = __etcd_get(key, &node, 0);
        if(ret){
                GOTO(err_ret, ret);
        }

        LTG_ASSERT(node->key && node->value == NULL);

        __etcd_readdir(key, node, buf, buflen);

        free_etcd_node(node);

        return 0;
err_ret:
        return ret;
}

static void __etcd_list(const char *prefix, etcd_node_t *list)
{
        int i, len;
        char tmp[MAX_NAME_LEN];
        const char *key;
        etcd_node_t *node;

        len = strlen(prefix) + 1;
        for (i = 0; i < list->num_node; i++) {
                node = list->nodes[i];

                key = node->key + len;
                //reclen = sizeof(*de) + strlen(key) + 1;

                strcpy(tmp, key);
                DBUG("convert from %s to %s\n", node->key, tmp);
                strcpy(node->key, tmp);
        }
}

int etcd_list(const char *_key, etcd_node_t **_node)
{
        int ret;
        char key[MAX_PATH_LEN];
        etcd_node_t *node = NULL;

        snprintf(key, MAX_NAME_LEN, "/%s/%s", ltgconf_global.system_name, _key);
        ret = __etcd_get(key, &node, 0);
        if(ret){
                GOTO(err_ret, ret);
        }

        LTG_ASSERT(node->key && node->value == NULL);

        __etcd_list(key, node);
        *_node = node;

        return 0;
err_ret:
        return ret;
}

int etcd_list1(const char *prefix, const char *_key, etcd_node_t **_node)
{
        int ret;
        char key[MAX_PATH_LEN];
        etcd_node_t *node = NULL;

        snprintf(key, MAX_NAME_LEN, "/%s/%s/%s", ltgconf_global.system_name, prefix, _key);
        ret = __etcd_get(key, &node, 0);
        if(ret){
                GOTO(err_ret, ret);
        }

        LTG_ASSERT(node->key && node->value == NULL);

        __etcd_list(key, node);
        *_node = node;

        return 0;
err_ret:
        return ret;
}

int etcd_lock_init(etcd_lock_t *lock, const char *prefix, const char *key,
                   int ttl, uint32_t magic, int update)
{
        int ret;

        LTG_ASSERT(ttl > 0 && ttl < 30);
        
        ret = sem_init(&lock->sem, 0, 0);
        if (ret)
                GOTO(err_ret, ret);

        ret = sem_init(&lock->stoped, 0, 0);
        if (ret)
                GOTO(err_ret, ret);

        LTG_ASSERT(strlen(key) + 1 <= MAX_PATH_LEN);
        snprintf(lock->key, MAX_NAME_LEN, "/%s/%s/%s", ltgconf_global.system_name, prefix, key);

        ret = gethostname(lock->hostname, MAX_NAME_LEN);
        if (ret)
                GOTO(err_ret, ret);

        lock->ttl = ttl;
        lock->update = update;
        lock->running = 1;
        lock->retval = 0;
        lock->magic = magic;

        return 0;
err_ret:
        return ret;
}

static int __etcd_lock__(const char *key, const char *value,
                      const etcd_prevcond_t *precond, unsigned int ttl)
{
        int ret, used, retry = 0;
        time_t begin = gettime();

retry:
        ret = __etcd_set(key, value, precond, 0, ttl);
        if (ret) {
                retry++;
                used = gettime() - begin;

                if (ret == EAGAIN) {
                        goto retry;
                } else {
                        DWARN("update lock %s ttl %u fail, used %u, ret %u\n",
                                        key, ttl, used, ret);
                        GOTO(err_ret, ret);
                }
        }

        if (retry) {
                DINFO("lock %s retry %u success, used %u\n", key, retry, used);
        }

        return 0;
err_ret:
        return ret;
}

static void *__etcd_lock(void *arg)
{
        int ret;
        etcd_prevcond_t precond;
        char key[MAX_PATH_LEN], value[MAX_PATH_LEN];
        etcd_lock_t *lock = arg;
        struct timespec t;
        long nsec;
        nid_t nid = *net_getnid();

        strcpy(key, lock->key);

        snprintf(value, MAX_NAME_LEN, "%s,%u,%u", lock->hostname, nid.id, lock->magic);
        precond.type = prevValue;
        precond.value = value;

        LTG_ASSERT(lock->running);
        while (lock->running) {
                DBUG("update lock %s ttl %u\n", lock->key, lock->ttl);
                ret = __etcd_lock__(key, value, &precond, lock->ttl);
                if (ret) {
                        lock->retval = ret;
                        lock->running = 0;
                        ret = EPERM;
                        GOTO(err_ret, ret);
                }

                clock_gettime(CLOCK_REALTIME, &t);

                /* update every second, we get lock fail only 1second later */
                if (lock->update == -1) {
                        nsec = t.tv_nsec + (long)lock->ttl * (1000 * 1000 * 1000) / 2;
                } else {
                        nsec = t.tv_nsec + (long)lock->update * (1000 * 1000 * 1000);
                }

                t.tv_sec += nsec / (1000 * 1000 * 1000);
                t.tv_nsec = nsec % (1000 * 1000 * 1000);
                DBUG("t %llu,%llu\n", (LLU)t.tv_sec, (LLU)t.tv_nsec);

                ret = _sem_timedwait(&lock->sem, &t);
                if (ret) {
                        if (ret == ETIMEDOUT)
                                continue;
                        else
                                UNIMPLEMENTED(__DUMP__);
                }
        }

        sem_post(&lock->stoped);
        pthread_exit(NULL);
err_ret:
        sem_post(&lock->stoped);
        pthread_exit(NULL);
}

int etcd_lock(etcd_lock_t *lock)
{
        int ret;
        pthread_t th;
        pthread_attr_t ta;
        etcd_prevcond_t precond;
        char key[MAX_PATH_LEN], value[MAX_BUF_LEN];
        nid_t nid = *net_getnid();

        strcpy(key, lock->key);
        snprintf(value, MAX_NAME_LEN, "%s,%u,%u", lock->hostname, nid.id, lock->magic);
        precond.type = prevExist;
        precond.value = "false";
        lock->running = 1;
        lock->retval = 0;

        DINFO("lock %s %s ttl %d\n", lock->key, value, lock->ttl);
        ret = __etcd_set(key, value, &precond, 0, lock->ttl);
        if (ret) {
                DBUG("lock %s fail\n", lock->key);
                GOTO(err_ret, ret);
        }

        LTG_ASSERT(lock->running);

        (void) pthread_attr_init(&ta);
        (void) pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);
        ret = pthread_create(&th, &ta, __etcd_lock, lock);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int etcd_unlock(etcd_lock_t *lock)
{
        int ret;
        etcd_session  sess;
        char key[MAX_PATH_LEN], *host;

        if (etcd_lock_health(lock)) {
                ret = EPERM;
                GOTO(err_ret, ret);
        }

        lock->running = 0;
        ret = sem_post(&lock->sem);
        if (ret)
                GOTO(err_ret, ret);

        ret = sem_wait(&lock->stoped);
        if (ret)
                GOTO(err_ret, ret);

        host = strdup(__ETCD_SRV__);
        ret = __etcd_open_str(host, &sess);
        if (ret) {
                GOTO(err_free, ret);
        }


        strcpy(key, lock->key);

        ret = etcd_delete(sess,key);
        if (ret) {
                GOTO(err_close, ret);
        }

        etcd_close_str(sess);

        free(host);
        return 0;
err_close:
        etcd_close_str(sess);
err_free:
        free(host);
err_ret:
        return ret;
}

int etcd_locker(etcd_lock_t *lock, char *locker, nid_t *nid, uint32_t *_magic, int *idx)
{
        int ret;
        etcd_node_t *node = NULL;
        uint32_t magic;

        ret = __etcd_get(lock->key, &node, 1);
        if(ret){
                DINFO("%s not found\n", lock->key);
                GOTO(err_ret, ret);
        }

        if (node->dir) {
                ret = EISDIR;
                GOTO(err_free, ret);
        }

        LTG_ASSERT(node->key && node->value);
        if (idx) {
                *idx = node->modifiedIndex;
        }

        ret = sscanf(node->value, "%[^,],%hu, %u", locker, &nid->id, &magic);
        if (ret != 3) {
                ret = EIO;
                GOTO(err_free, ret);
        }

        DBUG("lock %s idx %u locker %s\n", lock->key, node->modifiedIndex, node->value);
        
        if (_magic) {
                *_magic = magic;
        }
        
        free_etcd_node(node);

        return 0;
err_free:
        free_etcd_node(node);
err_ret:
        return ret;
}

int etcd_lock_watch(etcd_lock_t *lock, char *locker, nid_t *nid, uint32_t *magic, int *idx)
{
        int ret;
        etcd_node_t  *node = NULL;
        etcd_session  sess;
        char *host;

        LTG_ASSERT(sche_self() == 0);

        host = strdup(__ETCD_SRV__);
        ret = __etcd_open_str(host, &sess);
        if (ret) {
                GOTO(err_ret, ret);
        }
        
        ret = etcd_watch(sess, lock->key, idx, &node, 0);
        if(ret != ETCD_OK){
                ret = EPERM;
                GOTO(err_close, ret);
        }

        LTG_ASSERT(node->dir == 0);
        free_etcd_node(node);

        ret = __etcd_get(lock->key, &node, 1);
        if(ret){
                GOTO(err_close, ret);
        }

        LTG_ASSERT(node->dir == 0);
        LTG_ASSERT(node->key && node->value);

        strcpy(locker, node->value);

        ret = sscanf(node->value, "%[^,],%hu, %u", locker, &nid->id, magic);
        if (ret != 3) {
                ret = EIO;
                GOTO(err_close, ret);
        }

        DBUG("lock %s idx %u locker %s\n", lock->key, node->modifiedIndex, node->value);

        LTG_ASSERT(strcmp(locker, ""));

        if (idx)
                *idx = node->modifiedIndex;

        etcd_close_str(sess);
        free_etcd_node(node);
        free(host);
        
        return 0;
err_close:
        etcd_close_str(sess);
err_ret:
        free(host);
        return ret;
}

int etcd_lock_health(etcd_lock_t *lock)
{
        if (lock->running == 1 && lock->retval == 0)
                return 1;
        else {
                DINFO("lock %s running %u retval %u\n", lock->key,
                      lock->running, lock->retval);
                return 0;
        }
}

int etcd_del2(char *key)
{
        int ret;
        etcd_session  sess;
        char *host;

        // LTG_ASSERT(sche_self() == 0);

        host = strdup(__ETCD_SRV__);
        ret = __etcd_open_str(host, &sess);
        if (ret) {
                GOTO(err_ret, ret);
        }

        DINFO("remove %s\n", key);
        ret = __etcd_del(sess, key);
        if (ret) {
                GOTO(err_close, ret);
        }

        etcd_close_str(sess);
        free(host);

        return 0;
err_close:
        etcd_close_str(sess);
err_ret:
        free(host);
        return ret;
}

int etcd_del(const char *prefix, const char *_key)
{
        char key[MAX_PATH_LEN];

        snprintf(key, MAX_NAME_LEN, "/%s/%s/%s", ltgconf_global.system_name, prefix, _key);

        return etcd_del2(key);
}

int etcd_del_dir(const char *prefix, const char *_key, int recursive)
{
        int ret;
        etcd_session  sess;
        char key[MAX_PATH_LEN], *host;

        // LTG_ASSERT(sche_self() == 0);

        host = strdup(__ETCD_SRV__);
        ret = __etcd_open_str(host, &sess);
        if (ret) {
                GOTO(err_ret, ret);
        }

        snprintf(key, MAX_NAME_LEN, "/%s/%s/%s", ltgconf_global.system_name, prefix, _key);

        DINFO("remove dir %s, recursive: %d\n", key, recursive);
        ret = __etcd_del_dir(sess, key, recursive);
        if (ret) {
                GOTO(err_close, ret);
        }

        etcd_close_str(sess);
        free(host);

        return 0;
err_close:
        etcd_close_str(sess);
err_ret:
        free(host);
        return ret;
}

struct sche_thread_ops etcd_ops = {
        .type           = SCHE_THREAD_ETCD,
        .begin_trans    = NULL,
        .commit_trans   = NULL,
};

static int __etcd_ops_register()
{
        return sche_thread_ops_register(&etcd_ops, etcd_ops.type, 3);
}

int etcd_init()
{
        int ret;

        ret = __etcd_ops_register();
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        return 0;
err_ret:
        return ret;
}

int etcd_set_text(const char *prefix, const char *_key, const char *_value, int flag, int ttl)
{
        int ret;
        char key[MAX_PATH_LEN], value[MAX_PATH_LEN];
        etcd_prevcond_t *precond, _precond;

        //LTG_ASSERT(strcmp(_value, ""));

        snprintf(key, MAX_NAME_LEN, "/%s/%s/%s", ltgconf_global.system_name, prefix, _key);
        strcpy(value, _value);

        precond = NULL;        
        if (flag & O_EXCL) {
                _precond.type = prevExist;
                _precond.value = "false";
                precond = &_precond;
        }

        ret = __etcd_set(key, value, precond, 0, ttl);
        if (ret) {
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

int etcd_set_bin(const char *prefix, const char *_key, const void *_value,
                 int valuelen, int flag, int ttl)
{
        int ret;
        char buf[MAX_BUF_LEN];
        size_t size;

        size = MAX_BUF_LEN;
        ret = urlsafe_b64_encode(_value, valuelen, buf, &size);
        LTG_ASSERT(ret == 0);

        ret = etcd_set_text(prefix, _key, buf, flag, ttl);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}
