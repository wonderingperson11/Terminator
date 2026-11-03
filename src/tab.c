/*
 * tab.c - spawning a tab's shell as a thread or a process (see tab.h).
 */
#include "tab.h"
#include "shell.h"

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    int in, out;
    char *dir;
} ThreadArg;

static void *shell_thread(void *p)
{
    ThreadArg *a = p;
#ifdef __linux__
    /* Give this thread its own working directory (fs_struct), so `cd` in
     * one tab does not move the others even though they share a process.
     * This is the same per-thread-cwd technique Samba uses. */
    if (unshare(CLONE_FS) != 0) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "terminator: unshare(CLONE_FS) failed (%s): tabs share one "
                            "working directory\n", strerror(errno));
    }
#endif
    Shell *sh = shell_new_interactive(a->in, a->out, 0, a->dir);
    shell_interactive_loop(sh);
    shell_free(sh);
    close(a->in);
    close(a->out);
    free(a->dir);
    free(a);
    return NULL;
}

int tabconn_spawn(TabConn *tc, int process_mode, const char *start_dir, const int *fds_to_close,
                  int nclose)
{
    memset(tc, 0, sizeof *tc);
    tc->process_mode = process_mode;
    mr_init(&tc->rd);
    buf_init(&tc->txq);
    int down[2], up[2];
    if (make_pipe(down, 1) < 0)
        return -1;
    if (make_pipe(up, 1) < 0) {
        close(down[0]);
        close(down[1]);
        return -1;
    }
    tc->to_shell = down[1];
    tc->from_shell = up[0];
    set_nonblock(tc->to_shell, 1);
    set_nonblock(tc->from_shell, 1);

    if (process_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            close(down[0]);
            close(down[1]);
            close(up[0]);
            close(up[1]);
            return -1;
        }
        if (pid == 0) {
            /* The tab process: it never touches X11 and keeps only its own
             * two pipe ends. */
            for (int i = 0; i < nclose; i++)
                if (fds_to_close[i] >= 0)
                    close(fds_to_close[i]);
            close(down[1]);
            close(up[0]);
            Shell *sh = shell_new_interactive(down[0], up[1], 1, start_dir);
            shell_interactive_loop(sh);
            _exit(sh->exit_code & 0xff);
        }
        tc->pid = pid;
        close(down[0]);
        close(up[1]);
        return 0;
    }

    ThreadArg *a = xcalloc(1, sizeof *a);
    a->in = down[0];
    a->out = up[1];
    a->dir = xstrdup(start_dir ? start_dir : "");
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 << 20);
    /* signals are handled by whichever thread; keep the GUI thread's mask */
    int r = pthread_create(&tc->thread, &attr, shell_thread, a);
    pthread_attr_destroy(&attr);
    if (r != 0) {
        free(a->dir);
        free(a);
        close(down[0]);
        close(down[1]);
        close(up[0]);
        close(up[1]);
        return -1;
    }
    tc->has_thread = 1;
    return 0;
}

int tabconn_flush(TabConn *tc)
{
    while (tc->txq.len > 0 && tc->to_shell >= 0) {
        ssize_t w = write(tc->to_shell, tc->txq.s, tc->txq.len);
        if (w > 0) {
            buf_erase(&tc->txq, 0, (size_t)w);
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && errno == EAGAIN)
            return 1;
        /* EPIPE: the shell is gone */
        buf_clear(&tc->txq);
        break;
    }
    return 0;
}

void tabconn_send(TabConn *tc, int type, const void *a, size_t alen, const void *b, size_t blen)
{
    if (tc->to_shell < 0)
        return;
    MsgHdr h = { (uint32_t)type, (uint32_t)(alen + blen) };
    buf_putn(&tc->txq, (const char *)&h, sizeof h);
    if (alen)
        buf_putn(&tc->txq, a, alen);
    if (blen)
        buf_putn(&tc->txq, b, blen);
    tabconn_flush(tc);
}

void tabconn_send_key(TabConn *tc, int action, const char *text, size_t len)
{
    int32_t a = action;
    tabconn_send(tc, M_KEY, &a, sizeof a, text, len);
}

void tabconn_send_resize(TabConn *tc, int cols, int rows)
{
    int32_t v[2] = { cols, rows };
    tabconn_send(tc, M_RESIZE, v, sizeof v, NULL, 0);
}

void tabconn_close_input(TabConn *tc)
{
    if (tc->to_shell >= 0) {
        close(tc->to_shell);
        tc->to_shell = -1;
    }
    buf_clear(&tc->txq);
}

void tabconn_finish(TabConn *tc)
{
    tabconn_close_input(tc);
    if (tc->from_shell >= 0) {
        close(tc->from_shell);
        tc->from_shell = -1;
    }
    if (tc->has_thread) {
        pthread_join(tc->thread, NULL);
        tc->has_thread = 0;
    }
    if (tc->pid > 0) {
        int st = 0;
        while (waitpid(tc->pid, &st, 0) < 0 && errno == EINTR)
            ;
        if (WIFSIGNALED(st)) {
            tc->crashed = 1;
            tc->crash_signal = WTERMSIG(st);
        } else if (WIFEXITED(st) && !tc->got_exit) {
            tc->exit_status = WEXITSTATUS(st);
        }
        tc->pid = 0;
    }
    mr_free(&tc->rd);
    buf_free(&tc->txq);
}
