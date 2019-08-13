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
#include "ltg_utils.h"

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

        ret = core_init(0, 0, 0);
        if (ret)
                GOTO(err_ret, ret);
        
        return 0;
err_ret:
        return ret;
}
