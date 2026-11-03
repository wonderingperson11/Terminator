/*
 * multiwatch.c - multiWatch ["cmd1", "cmd2", ...]
 *
 * Every command runs in its own process, in parallel, with its own pipe.
 * The shell waits on all read ends at once with poll() (no busy-waiting),
 * prints each command's output as it arrives in the required block format
 * and re-runs a command INTERVAL seconds after it finishes (2 s by
 * default, like watch).  Ctrl+C reaches the shell as an event from the X11
 * loop; the shell then kill()s every child and returns to the prompt.
 */
#include "shell.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DASHES "----------------------------------------------------"
#define SETTLE_MS 60 /* output arriving closer together is one block */

typedef struct {
    char *cmd;
    pid_t pid;
    int fd;
    Buf pending;
    long ts;
    long long last_data;
    long long next_start;
} Watch;

/* Parse ["cmd1", "cmd2", ...] (double or single quotes, \-escapes). */
static int parse_list(const char *s, StrVec *out, char *err, size_t errlen)
{
    const char *p = s;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '[') {
        snprintf(err, errlen, "expected '['");
        return -1;
    }
    p++;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        if (*p == ']')
            break;
        if (*p != '"' && *p != '\'') {
            snprintf(err, errlen, "expected a quoted command near '%.10s'", p);
            return -1;
        }
        char q = *p++;
        Buf b;
        buf_init(&b);
        while (*p && *p != q) {
            if (q == '"' && *p == '\\' && (p[1] == '"' || p[1] == '\\')) {
                buf_putc(&b, p[1]);
                p += 2;
            } else {
                buf_putc(&b, *p++);
            }
        }
        if (*p != q) {
            buf_free(&b);
            snprintf(err, errlen, "unterminated quote");
            return -1;
        }
        p++;
        sv_push(out, buf_detach(&b));
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == ']')
            break;
        snprintf(err, errlen, "expected ',' or ']'");
        return -1;
    }
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p) {
        snprintf(err, errlen, "unexpected text after ']'");
        return -1;
    }
    return 0;
}

static void start(Shell *sh, Watch *w, int interactive)
{
    int p[2];
    if (make_pipe(p, 1) < 0)
        return;
    pid_t pid = shell_fork(sh);
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return;
    }
    if (pid == 0) {
        if (interactive)
            setpgid(0, 0);
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) {
            dup2(dn, 0);
            close(dn);
        }
        dup2(p[1], 1);
        dup2(p[1], 2);
        shell_enter_subshell(sh);
        subshell_run_string(sh, w->cmd);
    }
    if (interactive)
        setpgid(pid, pid);
    close(p[1]);
    set_nonblock(p[0], 1);
    w->pid = pid;
    w->fd = p[0];
}

static void flush(Shell *sh, Watch *w)
{
    if (w->pending.len == 0)
        return;
    Buf b;
    buf_init(&b);
    buf_printf(&b, "\"%s\", %ld:\n%s\n", w->cmd, w->ts, DASHES);
    buf_putn(&b, w->pending.s, w->pending.len);
    if (w->pending.s[w->pending.len - 1] != '\n')
        buf_putc(&b, '\n');
    buf_printf(&b, "%s\n", DASHES);
    sh_write(sh, sh->fd_out, b.s, b.len);
    buf_free(&b);
    buf_clear(&w->pending);
}

static void reap(Watch *w, int block)
{
    if (w->pid <= 0)
        return;
    int st;
    pid_t r;
    do {
        r = waitpid(w->pid, &st, block ? 0 : WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == w->pid || (r < 0 && errno == ECHILD))
        w->pid = 0;
}

int builtin_multiwatch(Shell *sh, int argc, char **argv, char **raw)
{
    (void)raw;
    int i = 1;
    double interval = 2.0;
    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        char *v = expand_str(sh, argv[2]);
        interval = atof(v);
        free(v);
        if (interval < 0.1)
            interval = 0.1;
        i = 3;
    }
    if (i >= argc) {
        sh_errorf(sh, "multiWatch: usage: multiWatch [-n secs] [\"cmd1\", \"cmd2\", ...]");
        return 2;
    }

    /* argv holds the raw words; rebuild the list text */
    Buf text;
    buf_init(&text);
    for (int k = i; k < argc; k++) {
        if (k > i)
            buf_putc(&text, ' ');
        buf_puts(&text, argv[k]);
    }
    StrVec cmds;
    sv_init(&cmds);
    char err[128];
    const char *t = text.s;
    while (*t == ' ')
        t++;
    if (*t == '[') {
        if (parse_list(t, &cmds, err, sizeof err) < 0) {
            sh_errorf(sh, "multiWatch: %s", err);
            buf_free(&text);
            sv_free(&cmds);
            return 2;
        }
    } else {
        for (int k = i; k < argc; k++)
            sv_push(&cmds, expand_str(sh, argv[k]));
    }
    buf_free(&text);
    if (cmds.n == 0) {
        sh_errorf(sh, "multiWatch: no commands given");
        sv_free(&cmds);
        return 2;
    }

    int interactive = sh->interactive;
    if (interactive) {
        sh->ed.mode = ED_OFF;
        ed_hide(sh);
    }
    int n = cmds.n;
    Watch *w = xcalloc((size_t)n, sizeof *w);
    long long now = now_ms();
    for (int k = 0; k < n; k++) {
        w[k].cmd = cmds.v[k];
        w[k].fd = -1;
        buf_init(&w[k].pending);
        w[k].next_start = now;
    }
    long long interval_ms = (long long)(interval * 1000);

    int stop = 0;
    while (!stop) {
        now = now_ms();
        int timeout = -1;
        for (int k = 0; k < n; k++) {
            Watch *x = &w[k];
            if (x->fd < 0 && x->pid > 0)
                reap(x, 0);
            if (x->fd < 0 && x->pid == 0 && x->next_start && now >= x->next_start) {
                x->next_start = 0;
                start(sh, x, interactive);
            }
            if (x->pending.len && now - x->last_data >= SETTLE_MS)
                flush(sh, x);
            long long due = -1;
            if (x->pending.len)
                due = x->last_data + SETTLE_MS - now;
            else if (x->fd < 0 && x->pid == 0 && x->next_start)
                due = x->next_start - now;
            else if (x->fd < 0 && x->pid > 0)
                due = 50; /* exited child not reaped yet (non-interactive) */
            if (due >= 0 && (timeout < 0 || due < timeout))
                timeout = (int)(due < 0 ? 0 : due);
        }

        if (interactive) {
            int ev = shell_poll_input(sh);
            if (ev == EV_INTR || ev == EV_CLOSE)
                break;
            if (ev != EV_NONE)
                continue;
        }

        int fds[64], idx[64], nf = 0;
        for (int k = 0; k < n && nf < 64; k++) {
            if (w[k].fd >= 0) {
                fds[nf] = w[k].fd;
                idx[nf++] = k;
            }
        }
        short rev[64];
        if (interactive) {
            shell_wait(sh, fds, nf, rev, timeout);
        } else {
            struct pollfd pfd[64];
            for (int k = 0; k < nf; k++) {
                pfd[k].fd = fds[k];
                pfd[k].events = POLLIN;
                pfd[k].revents = 0;
            }
            if (poll(pfd, (nfds_t)nf, timeout) < 0 && errno != EINTR)
                stop = 1;
            for (int k = 0; k < nf; k++)
                rev[k] = pfd[k].revents;
        }
        for (int k = 0; k < nf; k++) {
            if (!(rev[k] & (POLLIN | POLLHUP | POLLERR)))
                continue;
            Watch *x = &w[idx[k]];
            char tmp[8192];
            ssize_t r = read(x->fd, tmp, sizeof tmp);
            if (r > 0) {
                if (x->pending.len == 0)
                    x->ts = (long)time(NULL);
                buf_putn(&x->pending, tmp, (size_t)r);
                x->last_data = now_ms();
                if (x->pending.len >= 65536)
                    flush(sh, x); /* never-ending output: don't hoard it */
            } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
                /* the command finished: show what it printed, schedule
                 * the next run */
                close(x->fd);
                x->fd = -1;
                flush(sh, x);
                reap(x, !interactive);
                x->next_start = now_ms() + interval_ms;
            }
        }
    }

    /* Ctrl+C: terminate every child and return to the prompt */
    for (int k = 0; k < n; k++) {
        if (w[k].pid > 0) {
            kill(interactive ? -w[k].pid : w[k].pid, SIGINT);
            kill(interactive ? -w[k].pid : w[k].pid, SIGKILL);
        }
    }
    for (int k = 0; k < n; k++) {
        if (w[k].pid > 0)
            reap(&w[k], 1);
        if (w[k].fd >= 0)
            close(w[k].fd);
        flush(sh, &w[k]);
        buf_free(&w[k].pending);
    }
    sh_out(sh, "^C\n");
    free(w);
    sv_free(&cmds);
    return 130;
}
