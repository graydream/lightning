#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_NET

#include "ltg_core.h"
#include "ltg_net.h"
#include "ltg_rpc.h"
#include "ltg_utils.h"
#include "3part.h"

static int __conn_add(const nid_t *nid)
{
        int ret;
        char key[MAX_NAME_LEN], buf[MAX_BUF_LEN], tmp[MAX_BUF_LEN];
        ltg_net_info_t *info;
        net_handle_t nh;
        size_t len;

        if (netable_connected(nid)) {
                goto out;
        }

        snprintf(key, MAX_NAME_LEN, "%u.info", nid->id);

        ret = etcd_get_text(ETCD_MANAGE, key, tmp, NULL);
        if (ret) {
                goto out;
        }

        len = MAX_BUF_LEN;
        info = (void *)buf;
        ret = urlsafe_b64_decode(tmp, strlen(tmp), (void *)info, &len);
        LTG_ASSERT(ret == 0);        

        DBUG("connect to %u %s\n", nid->id, info->name);

        ret = netable_connect(&nh, info);
        if (ret) {
                DBUG("connect to %u %s fail\n", nid->id, info->name);
                GOTO(err_ret, ret);
        }

out:

        return 0;
err_ret:
        return ret;
}

int conn_scan()
{
        int ret, i;
        etcd_node_t *list = NULL, *node;
        nid_t nid;

        ret = etcd_list(ETCD_MANAGE, &list);
        if (unlikely(ret)) {
                if (ret == ENOKEY) {
                        DINFO("conn table empty\n");
                        goto out;
                } else
                        GOTO(err_ret, ret);
        }

        for(i = 0; i < list->num_node; i++) {
                node = list->nodes[i];
 
                if (strstr(node->key, ".info") == NULL) {
                        DBUG("skip %s\n", node->key);
                        continue;
                }

                str2nid(&nid, node->key);
                ret = __conn_add(&nid);
        }

        free_etcd_node(list);

out:
        return 0;
err_ret:
        return ret;
}

inline static int __conn_watch(int idx, void *arg)
{
        int ret;
        
        (void) arg;

        static int __idx__ = 0;
        if (__idx__ < idx) {
                __idx__ = idx;
                DINFO("new conn idx %d\n", idx);
        }
        
        ret = conn_scan();
        if (ret) {
                GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

static void *__conn_scan(void *arg)
{
        (void) arg;

        while (1) {
#if 1
                int ret;

                ret = etcd_watch_dir(ETCD_NETWORK, "manage", 60,
                                     __conn_watch, NULL);
                if (unlikely(ret)) {
                        sleep(1);
                        continue;
                }
#else
                sleep(10);
                conn_scan();
#endif
        }

        pthread_exit(NULL);
}

int conn_init()
{
        int ret;

        ret = ltg_thread_create(__conn_scan, NULL, "__conn_scan");
        if (unlikely(ret))
                GOTO(err_ret, ret);
        
        return 0;
err_ret:
        return ret;
}

static int __conn_init_info(nid_t *_nid)
{
        int ret, retry = 0;
        char key[MAX_NAME_LEN], buf[MAX_BUF_LEN], tmp[MAX_BUF_LEN];
        ltg_net_info_t *info;
        uint32_t buflen;
        size_t size;
        nid_t nid;

        info = (void *)buf;
        buflen = MAX_BUF_LEN;
        ret = rpc_getinfo(buf, &buflen);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        LTG_ASSERT(info->len);
        LTG_ASSERT(info->info_count);
        size = MAX_BUF_LEN;
        ret = urlsafe_b64_encode((void *)info, info->len, tmp, &size);
        LTG_ASSERT(ret == 0);

        nid = *net_getnid();
        LTG_ASSERT(nid.id == info->id.id);
        snprintf(key, MAX_NAME_LEN, "%u.info", nid.id);

retry:
        DBUG("register %s value %s\n", key, tmp);
        ret = etcd_create_text(ETCD_MANAGE, key, tmp, 0);
        if (unlikely(ret)) {
                ret = etcd_update_text(ETCD_MANAGE, key, tmp, NULL, 0);
                if (unlikely(ret)) {
                        USLEEP_RETRY(err_ret, ret, retry, retry, 30, (1000 * 1000));
                }
        }

        *_nid = nid;

        return 0;
err_ret:
        return ret;
}

int conn_register()
{
        int ret;
        nid_t nid;

        ret = __conn_init_info(&nid);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int conn_getinfo(const nid_t *nid, ltg_net_info_t *info)
{
        int ret;
        char key[MAX_NAME_LEN], tmp[MAX_BUF_LEN];
        size_t  len;

        snprintf(key, MAX_NAME_LEN, "%u.info", nid->id);
        ret = etcd_get_text(ETCD_MANAGE, key, tmp, NULL);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        DBUG("get %s value %s\n", key, tmp);
        len = MAX_BUF_LEN;
        ret = urlsafe_b64_decode(tmp, strlen(tmp), (void *)info, &len);
        LTG_ASSERT(ret == 0);
        LTG_ASSERT(info->info_count * sizeof(sock_info_t) + sizeof(ltg_net_info_t) == info->len);
        LTG_ASSERT(info->info_count);

        return 0;
err_ret:
        return ret;
}
