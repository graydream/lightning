#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <getopt.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <sys/types.h>

#include "ltg_core.h"
#include "ltg_lib.h"

int main(int argc, char *argv[])
{
        int ret;
        char c_opt, *prog;

        prog = strrchr(argv[0], '/');
        if (prog)
                prog++;
        else
                prog = argv[0];

        if (argc < 2) {
                exit(1);
        }

        while (srv_running) {
                int option_index = 0;

                static struct option long_options[] = {
                        { 0, 0, 0, 0 },
                };

                c_opt = getopt_long(argc, argv, "varf:t:n:", long_options, &option_index);
                if (c_opt == -1)
                        break;

                switch (c_opt) {
                default:
                        exit(1);
                }
        }

        if (optind >= argc) {
                exit(1);
        }
        
        ltgconf_t ltgconf;
        ltg_netconf_t ltgnet_conf;

        ltg_conf_init(&ltgconf);

        strcpy(ltgconf.system_name, "ltg_example");
        strcpy(ltgconf.service_name, "create");
        strcpy(ltgconf.workdir, "/tmp/example");

        ltgconf.coremask = 0x10001;
        ltgconf.rpc_timeout = 10;
        ltgconf.backtrace = 0;
        ltgconf.daemon = 1;
        ltgconf.coreflag = CORE_FLAG_POLLING;

#if 0
        for (int i = 0; i < netconf_backend.count; i++) {
                ltgnet_conf.network[i].network = netconf_backend.network[i].network;
                ltgnet_conf.network[i].mask = netconf_backend.network[i].mask;
                ltgnet_conf.count++;
        }
#endif
        
        ret = ltg_init(&ltgconf, &ltgnet_conf, &ltgnet_conf);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}
