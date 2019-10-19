#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_net.h"
#include "ltg_utils.h"
#include "ltg_core.h"

typedef struct {
        ltg_spinlock_t lock;
        char prefix[MAX_PATH_LEN];
} maping_t;

static maping_t maping;

static inline int str2netinfo(ltg_net_info_t *info, const char *buf)
{
        int ret;
        const char *addrs;
        char addr[MAX_NAME_LEN];
        uint32_t port, i;

        memset(info, 0x0, sizeof(*info));

        ret = sscanf(buf,
                     "len:%d\n"
                     "uptime:%u\n"
                     "nid:%hu\n"
                     "hostname:%[^\n]\n"
                     "magic:%d\n"
                     "info_count:%"SCNd16"\n"
                     "info:",
                     &info->len,
                     &info->uptime,
                     &info->id.id,
                     info->name,
                     &info->magic,
                     &info->info_count);
        if (ret != 6) {
                UNIMPLEMENTED(__DUMP__);
                ret = EAGAIN;
                GOTO(err_ret, ret);
        }

        addrs = strstr(buf, "info:") + 5;
        for (i = 0; i < info->info_count; ++i) {
                ret = sscanf(addrs, "%[^/]/%d", addr, &port);
                LTG_ASSERT(ret == 2);
                addrs = strchr(addrs, ',') + 1;

                info->info[i].addr = inet_addr(addr);
                info->info[i].port = htons(port);
        }

        return 0;
err_ret:
        return ret;
}

static inline void netinfo2str(char *buf, const ltg_net_info_t *info)
{
        uint32_t i;
        const sock_info_t *sock;

        snprintf(buf, MAX_NAME_LEN,
                 "len:%d\n"
                 "uptime:%u\n"
                 "nid:"NID_FORMAT"\n"
                 "hostname:%s\n"
                 "magic:%d\n"
                 "info_count:%u\n"
                 "info:",
                 info->len,
                 info->uptime,
                 NID_ARG(&info->id),
                 info->name,
                 info->magic,
                 info->info_count);

        LTG_ASSERT(strlen(info->name));
        LTG_ASSERT(info->info_count * sizeof(sock_info_t)
                + sizeof(ltg_net_info_t) == info->len);
        
        for (i = 0; i < info->info_count; i++) {
                sock = &info->info[i];
                snprintf(buf + strlen(buf), MAX_NAME_LEN, "%s/%u,",
                         _inet_ntoa(sock->addr), ntohs(sock->port));
        }

        //DINFO("\n%s\n", buf);
}

static void __replace(char *new, const char *old, char from, char to)
{
        char *tmp;
        strcpy(new, old);
        tmp = strchr(new, from);
        *tmp = to;
}

int maping_init()
{
        int ret;

        DINFO("maping init\n");
        ret = ltg_spin_init(&maping.lock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        snprintf(maping.prefix, MAX_PATH_LEN, "/dev/shm/%s/maping", ltgconf_global.system_name);

        if (ltgconf_global.daemon) {
                ret = path_validate(maping.prefix, LLIB_ISDIR, LLIB_DIRCREATE);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

int maping_get(const char *type, const char *_key, char *value, time_t *ctime)
{
        int ret, retry = 0;
        char path[MAX_PATH_LEN], tmp[MAX_PATH_LEN], crc_value[MAX_PATH_LEN];
        const char *key;
        uint32_t crc, _crc;
        struct stat stbuf;

        if (strchr(_key, '/')) {
                __replace(tmp, _key, '/', ':');
                LTG_ASSERT(strchr(tmp, '/') == 0);
                key = tmp;
        } else {
                key = _key;
        }

        snprintf(path, MAX_NAME_LEN, "%s/%s/%s", maping.prefix, type, key);

        ret = ltg_spin_lock(&maping.lock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = _get_text(path, crc_value, MAX_BUF_LEN);
        if (ret < 0) {
                ret = -ret;
                goto err_lock;
        }

        if (ctime) {
                ret = stat(path, &stbuf);
                if (ret < 0) {
                        ret = errno;
                        GOTO(err_lock, ret);
                }      

                *ctime = stbuf.st_ctime;
        }
        
retry:
        if (!strcmp(NAME2NID, type)) {
                sscanf(crc_value, "%x %s", &crc, value);
                _crc = crc32_sum(value, strlen(value));
                if (_crc != crc) {
                        USLEEP_RETRY(err_unlink, ENOENT, retry, retry, 2, (100 * 1000));
                }
        } else {
                strcpy(value, crc_value);
        }
        //DWARN("path %s\n", path);

        ltg_spin_unlock(&maping.lock);

        return 0;
err_unlink:
        DWARN("remove %s\n", path);
        unlink(path);
err_lock:
        ltg_spin_unlock(&maping.lock);
err_ret:
        return ret;
}

int maping_set(const char *type, const char *_key, const char *value)
{
        int ret;
        char path[MAX_PATH_LEN], tmp[MAX_PATH_LEN], crc_value[MAX_PATH_LEN] = {0};
        const char *key;
        uint32_t crc;

        DBUG("set %s %s\n", type, _key);

        if (strchr(_key, '/')) {
                __replace(tmp, _key, '/', ':');
                LTG_ASSERT(strchr(tmp, '/') == 0);
                key = tmp;
        } else {
                key = _key;
        }

        snprintf(path, MAX_NAME_LEN, "%s/%s/%s", maping.prefix, type, key);

        crc = crc32_sum(value, strlen(value));
        if (!strcmp(NAME2NID, type)) {
                sprintf(crc_value, "%x %s", crc, value);
        } else {
                strcpy(crc_value, value);
        }

        ret = ltg_spin_lock(&maping.lock);
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        ret = _set_text(path, crc_value, strlen(crc_value), O_CREAT | O_TRUNC | O_SYNC);
        if (unlikely(ret))
                GOTO(err_lock, ret);

        ltg_spin_unlock(&maping.lock);

        return 0;
err_lock:
        ltg_spin_unlock(&maping.lock);
err_ret:
        return ret;
}

int maping_drop(const char *type, const char *_key)
{
        int ret;
        char path[MAX_PATH_LEN], tmp[MAX_PATH_LEN];
        const char *key;
        struct stat stbuf;

        DBUG("remove %s %s\n", type, _key);

        if (strchr(_key, '/')) {
                __replace(tmp, _key, '/', ':');
                LTG_ASSERT(strchr(tmp, '/') == 0);
                key = tmp;
        } else {
                key = _key;
        }

        snprintf(path, MAX_NAME_LEN, "%s/%s/%s", maping.prefix, type, key);

        ret = ltg_spin_lock(&maping.lock);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = stat(path, &stbuf);
        if (ret == 0) {
                DBUG("remove %s\n", path);
                unlink(path);
        }

        ltg_spin_unlock(&maping.lock);

        return 0;
err_ret:
        return ret;
}

static int __maping_setnetinfo__(const ltg_net_info_t *info)
{
        int ret;
        char buf[MAX_BUF_LEN], nidstr[MAX_NAME_LEN];

        netinfo2str(buf, info);
        nid2str(nidstr, &info->id);
        ret = maping_set(NID2NETINFO, nidstr, buf);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = maping_set(HOST2NID, info->name, nidstr);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int maping_nid2host(const nid_t *nid, char *hostname)
{
        int ret;
        char buf[MAX_BUF_LEN];
        ltg_net_info_t *info;

        info = (void *)buf;
        ret = maping_nid2netinfo(nid, info);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        strcpy(hostname, info->name);
        
        return 0;
err_ret:
        return ret;
}

int maping_nid2netinfo(const nid_t *nid, ltg_net_info_t *info)
{
        int ret;
        char buf[MAX_BUF_LEN], nidstr[MAX_NAME_LEN];
        time_t ctime;

        nid2str(nidstr, nid);
        ret = maping_get(NID2NETINFO, nidstr, buf, &ctime);
        if (unlikely(ret)) {
                LTG_ASSERT(ret == ENOENT);

        retry:
                ret = conn_getinfo(nid, info);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }

                ret = __maping_setnetinfo__(info);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        } else {
                if (gettime() -  ctime > ltgconf_global.rpc_timeout / 2) {
                        DBUG("drop %s\n", nidstr);
                        goto retry;
                }

                ret = str2netinfo(info, buf);
                LTG_ASSERT(ret == 0);
        }
        
        return 0;
err_ret:
        return ret;
}
