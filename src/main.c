#include "gui.h"
#include "headless.h"
#include "shell.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void usage(void)
{
    fputs("usage: terminator [useProcess=True|False]\n"
          "       terminator --headless [useProcess=True]   (tab without X11, reads stdin)\n"
          "       terminator -c 'command line'\n"
          "       terminator --script file [args...]\n",
          stderr);
}

static int parse_bool(const char *v)
{
    return strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcasecmp(v, "yes") == 0;
}

int main(int argc, char **argv)
{
    int process_mode = 0, headless = 0;
    const char *command = NULL, *script = NULL;
    int script_arg = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncasecmp(a, "useProcess=", 11) == 0) {
            process_mode = parse_bool(a + 11);
        } else if (strcmp(a, "--process") == 0) {
            process_mode = 1;
        } else if (strcmp(a, "--headless") == 0) {
            headless = 1;
        } else if (strcmp(a, "-c") == 0 && i + 1 < argc) {
            command = argv[++i];
        } else if (strcmp(a, "--script") == 0 && i + 1 < argc) {
            script = argv[++i];
            script_arg = i + 1;
            break;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "terminator: unknown argument '%s'\n", a);
            usage();
            return 2;
        }
    }

    hist_init();

    if (command || script) {
        Shell *sh = shell_new_batch();
        int st;
        if (command) {
            st = shell_run_string(sh, command, NULL);
        } else {
            st = run_script_file(sh, script, argv + script_arg, argc - script_arg, 0);
        }
        return st;
    }

    /* Terminator itself must never be stopped or killed by job-control
     * keys or a closed pipe; its children get the defaults back. */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    wake_install_handler();

    if (headless)
        return headless_run(process_mode);
    return gui_run(process_mode);
}
