#include "headless.h"
#include "proto.h"
#include "tab.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_HTABS 8
static TabConn tcs[MAX_HTABS];
static int prompts[MAX_HTABS];
static int ended[MAX_HTABS];
static int ntabs, cur;
static int pmode;
/* the current tab */
#define tc tcs[cur]
#define prompts_seen prompts[cur]
#define finished ended[cur]

static void handle(int k, uint32_t type, char *data, uint32_t len)
{
    switch (type) {
    case M_OUT:
        fwrite(data, 1, len, stdout);
        break;
    case M_COMMIT:
        fwrite(data + 4, 1, len - 4, stdout);
        fputc('\n', stdout);
        break;
    case M_EDIT:
        /* a fresh prompt: prompt text and an empty line */
        if (len >= 8 && msg_int(data, 0) > 0 && (uint32_t)msg_int(data, 0) == len - 8)
            prompts[k]++;
        break;
    case M_CLEAR:
        fputs("[screen cleared]\n", stdout);
        break;
    case M_EXIT:
        tcs[k].got_exit = 1;
        tcs[k].exit_status = len >= 4 ? msg_int(data, 0) : 0;
        break;
    default:
        break;
    }
}

static void pump(int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    for (;;) {
        long long left = end - now_ms();
        if (left < 0)
            left = 0;
        struct pollfd p[2 * MAX_HTABS];
        int who[2 * MAX_HTABS];
        int n = 0;
        for (int k = 0; k < ntabs; k++) {
            if (tcs[k].from_shell < 0)
                continue;
            p[n].fd = tcs[k].from_shell;
            p[n].events = POLLIN;
            who[n++] = k;
            if (tcs[k].txq.len) {
                p[n].fd = tcs[k].to_shell;
                p[n].events = POLLOUT;
                who[n++] = k;
            }
        }
        int r = poll(p, (nfds_t)n, (int)left);
        if (r < 0 && errno != EINTR)
            return;
        for (int i = 0; i < n && r > 0; i++) {
            int k = who[i];
            if (p[i].events & POLLOUT) {
                tabconn_flush(&tcs[k]);
                continue;
            }
            if (!(p[i].revents & (POLLIN | POLLHUP)))
                continue;
            long got = mr_fill(&tcs[k].rd, tcs[k].from_shell);
            uint32_t type, len;
            char *data;
            while (mr_next(&tcs[k].rd, &type, &data, &len))
                handle(k, type, data, len);
            if (got == 0) {
                ended[k] = 1;
                close(tcs[k].from_shell);
                tcs[k].from_shell = -1;
            }
        }
        fflush(stdout);
        if (finished || now_ms() >= end)
            return;
    }
}

static int open_tab(void)
{
    if (ntabs >= MAX_HTABS)
        return -1;
    int closefds[2 * MAX_HTABS], nclose = 0;
    for (int k = 0; k < ntabs; k++) {
        closefds[nclose++] = tcs[k].to_shell;
        closefds[nclose++] = tcs[k].from_shell;
    }
    if (tabconn_spawn(&tcs[ntabs], pmode, NULL, closefds, nclose) < 0)
        return -1;
    cur = ntabs++;
    tabconn_send_resize(&tc, 80, 24);
    return 0;
}

static void wait_prompt(int seen_before, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (!finished && prompts_seen == seen_before && now_ms() < end)
        pump(20);
    pump(30); /* collect stragglers */
}

static void key(int action, const char *text)
{
    tabconn_send_key(&tc, action, text, text ? strlen(text) : 0);
}

int headless_run(int process_mode)
{
    pmode = process_mode;
    if (open_tab() < 0) {
        perror("terminator: spawn");
        return 1;
    }
    wait_prompt(0, 5000);

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) >= 0) {
        if (n > 0 && line[n - 1] == '\n')
            line[--n] = '\0';
        int alive = 0;
        for (int k = 0; k < ntabs; k++)
            alive += !ended[k];
        if (!alive)
            break;
        int before = prompts_seen;
        if (finished && strncmp(line, "@SWITCH", 7) != 0 && strncmp(line, "@NEWTAB", 7) != 0)
            continue; /* the current tab is gone */
        if (line[0] == '@') {
            char *arg = strchr(line, ' ');
            if (arg)
                *arg++ = '\0';
            const char *cmd = line + 1;
            if (!strcmp(cmd, "INTR"))
                key(ACT_INTR, NULL);
            else if (!strcmp(cmd, "SUSP"))
                key(ACT_SUSP, NULL);
            else if (!strcmp(cmd, "EOF"))
                key(ACT_EOF, NULL);
            else if (!strcmp(cmd, "TAB"))
                key(ACT_COMPLETE, NULL);
            else if (!strcmp(cmd, "SEARCH"))
                key(ACT_SEARCH, NULL);
            else if (!strcmp(cmd, "UP"))
                key(ACT_HIST_PREV, NULL);
            else if (!strcmp(cmd, "DOWN"))
                key(ACT_HIST_NEXT, NULL);
            else if (!strcmp(cmd, "CLEAR"))
                key(ACT_CLEAR, NULL);
            else if (!strcmp(cmd, "ESC"))
                key(ACT_ESCAPE, NULL);
            else if (!strcmp(cmd, "KEY") && arg)
                key(atoi(arg), NULL);
            else if (!strcmp(cmd, "TYPE") && arg)
                key(ACT_INSERT, arg);
            else if (!strcmp(cmd, "ENTER"))
                key(ACT_ENTER, NULL);
            else if (!strcmp(cmd, "NOWAIT")) {
                if (arg)
                    key(ACT_INSERT, arg);
                key(ACT_ENTER, NULL);
            } else if (!strcmp(cmd, "SLEEP")) {
                pump(arg ? atoi(arg) : 100);
                continue;
            } else if (!strcmp(cmd, "WAIT")) {
                wait_prompt(before, 15000);
                continue;
            } else if (!strcmp(cmd, "NEWTAB")) {
                if (open_tab() == 0)
                    wait_prompt(0, 5000);
                continue;
            } else if (!strcmp(cmd, "SWITCH") && arg) {
                int k = atoi(arg) - 1;
                if (k >= 0 && k < ntabs)
                    cur = k;
                continue;
            }
            if (!strcmp(cmd, "ENTER") || !strcmp(cmd, "INTR") || !strcmp(cmd, "EOF"))
                wait_prompt(before, 3000);
            else
                pump(150);
            continue;
        }
        key(ACT_INSERT, line);
        key(ACT_ENTER, NULL);
        wait_prompt(before, 15000);
    }
    free(line);
    int status = 0;
    for (int k = ntabs - 1; k >= 0; k--) {
        cur = k;
        if (!finished)
            tabconn_close_input(&tc);
        while (!finished)
            pump(100);
        tabconn_finish(&tc);
        if (tc.crashed)
            printf("[tab process killed by signal %d]\n", tc.crash_signal);
        if (k == 0)
            status = tc.exit_status;
    }
    return status;
}
