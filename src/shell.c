/*
 * shell.c - shell lifecycle, the interactive read-eval loop, output to the
 * GUI and the plumbing used by every wait loop.
 */
#include "shell.h"

#include <errno.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static char *user_name(void)
{
    struct passwd pw, *res = NULL;
    char tmp[4096];
    if (getpwuid_r(getuid(), &pw, tmp, sizeof tmp, &res) == 0 && res)
        return xstrdup(pw.pw_name);
    const char *u = getenv("USER");
    return xstrdup(u && *u ? u : "user");
}

static void init_common(Shell *sh)
{
    sh->wake_slot = -1;
    sh->wake_fd = -1;
    sh->out_r = sh->out_w = -1;
    sh->gui_in = sh->gui_out = -1;
    sh->fd_in = 0;
    sh->fd_out = 1;
    sh->fd_err = 2;
    sh->shell_pid = getpid();
    sh->cols = 80;
    sh->rows = 24;
    vars_init_from_env(sh, environ);
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd))
        var_set(sh, "PWD", cwd, 1);
    if (!var_get(sh, "HOME"))
        var_set(sh, "HOME", home_dir(), 1);
    ed_init(&sh->ed);
    buf_init(&sh->pending);
    mr_init(&sh->rd);
}

Shell *shell_new_interactive(int gui_in, int gui_out, int process_mode, const char *start_dir)
{
    Shell *sh = xcalloc(1, sizeof *sh);
    init_common(sh);
    sh->interactive = 1;
    sh->process_mode = process_mode;
    sh->gui_in = gui_in;
    sh->gui_out = gui_out;
    set_nonblock(gui_in, 1);
    sh->wake_slot = wake_register(&sh->wake_fd);
    int p[2];
    if (make_pipe(p, 1) == 0) {
        sh->out_r = p[0];
        sh->out_w = p[1];
        set_nonblock(sh->out_r, 1);
    }
    sh->fd_in = -1;
    sh->fd_out = FD_GUI;
    sh->fd_err = FD_GUI;

    if (start_dir && *start_dir && chdir(start_dir) != 0) {
        /* the directory vanished: keep the inherited one */
    }
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd))
        var_set(sh, "PWD", cwd, 1);

    /* Output goes through pipes, not a tty: keep programs plain. */
    var_set(sh, "TERM", "dumb", 1);
    var_set(sh, "PAGER", "cat", 1);
    var_set(sh, "GIT_PAGER", "cat", 1);
    var_set(sh, "COLUMNS", "80", 1);
    var_set(sh, "LINES", "24", 1);
    var_set(sh, "TERMINATOR_MODE", process_mode ? "process" : "thread", 1);

    char *user = user_name();
    sh->main_prompt = xasprintf("%s@terminator> ", user);
    free(user);
    return sh;
}

Shell *shell_new_batch(void)
{
    Shell *sh = xcalloc(1, sizeof *sh);
    init_common(sh);
    sh->main_prompt = xstrdup("$ ");
    return sh;
}

void shell_free(Shell *sh)
{
    if (!sh)
        return;
    while (sh->jobs)
        job_remove(sh, sh->jobs);
    wake_unregister(sh->wake_slot);
    if (sh->out_r >= 0)
        close(sh->out_r);
    if (sh->out_w >= 0)
        close(sh->out_w);
    vars_free(sh);
    for (int i = 0; i < sh->posc; i++)
        free(sh->posv[i]);
    free(sh->posv);
    free(sh->arg0);
    free(sh->main_prompt);
    free(sh->ready_line);
    ed_free(&sh->ed);
    buf_free(&sh->pending);
    mr_free(&sh->rd);
    free(sh);
}

/* ------------------------------------------------------------- output */

void sh_forward_output(Shell *sh)
{
    if (sh->out_r < 0)
        return;
    char tmp[16384];
    /* bounded: with endless output the caller must still see Ctrl+C */
    for (int rounds = 0; rounds < 64; rounds++) {
        ssize_t r = read(sh->out_r, tmp, sizeof tmp);
        if (r > 0) {
            if (sh->interactive && !sh->closed)
                msg_send(sh->gui_out, M_OUT, tmp, (size_t)r, NULL, 0);
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        break;
    }
}

void sh_write(Shell *sh, int fd, const char *s, size_t n)
{
    if (n == 0)
        return;
    if (fd == FD_GUI) {
        if (sh->interactive) {
            if (sh->closed)
                return;
            sh_forward_output(sh); /* keep children's output in order */
            msg_send(sh->gui_out, M_OUT, s, n, NULL, 0);
            return;
        }
        fd = 1;
    }
    if (fd >= 0)
        write_all(fd, s, n);
}

void sh_out(Shell *sh, const char *s)
{
    sh_write(sh, sh->fd_out, s, strlen(s));
}

void sh_printf(Shell *sh, const char *fmt, ...)
{
    char stackbuf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stackbuf, sizeof stackbuf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof stackbuf) {
        sh_write(sh, sh->fd_out, stackbuf, (size_t)n);
        return;
    }
    char *big = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sh_write(sh, sh->fd_out, big, (size_t)n);
    free(big);
}

void sh_errorf(Shell *sh, const char *fmt, ...)
{
    Buf b;
    buf_init(&b);
    buf_puts(&b, "terminator: ");
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        buf_reserve(&b, (size_t)n);
        va_start(ap, fmt);
        vsnprintf(b.s + b.len, (size_t)n + 1, fmt, ap);
        va_end(ap);
        b.len += (size_t)n;
    }
    buf_putc(&b, '\n');
    sh_write(sh, sh->fd_err, b.s, b.len);
    buf_free(&b);
}

void sh_send_title(Shell *sh)
{
    if (!sh->interactive || sh->closed)
        return;
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd))
        snprintf(cwd, sizeof cwd, "?");
    char *t = collapse_home(cwd);
    msg_send(sh->gui_out, M_TITLE, t, strlen(t), NULL, 0);
    free(t);
}

/* -------------------------------------------------------------- input */

static int handle_msg(Shell *sh, uint32_t type, char *data, uint32_t len)
{
    switch (type) {
    case M_KEY:
        if (len < 4)
            return EV_NONE;
        return ed_handle(sh, msg_int(data, 0), data + 4, len - 4);
    case M_RESIZE:
        if (len >= 8) {
            sh->cols = msg_int(data, 0);
            sh->rows = msg_int(data, 1);
            char tmp[32];
            snprintf(tmp, sizeof tmp, "%d", sh->cols);
            var_set(sh, "COLUMNS", tmp, 1);
            snprintf(tmp, sizeof tmp, "%d", sh->rows);
            var_set(sh, "LINES", tmp, 1);
        }
        return EV_NONE;
    default:
        return EV_NONE;
    }
}

int shell_poll_input(Shell *sh)
{
    if (!sh->interactive)
        return EV_NONE;
    if (sh->closed)
        return EV_CLOSE;
    for (;;) {
        uint32_t type, len;
        char *data;
        while (mr_next(&sh->rd, &type, &data, &len)) {
            int ev = handle_msg(sh, type, data, len);
            if (ev != EV_NONE)
                return ev;
        }
        long r = mr_fill(&sh->rd, sh->gui_in);
        if (r == 0) {
            sh->closed = 1;
            return EV_CLOSE;
        }
        if (r < 0)
            return EV_NONE;
    }
}

/* Block until the GUI, a child (SIGCHLD), children's output or one of the
 * extra descriptors needs attention.  Never busy-waits. */
int shell_wait(Shell *sh, int *fds, int nfds, short *revents, int timeout_ms)
{
    struct pollfd pfd[40];
    int k = 0, gui = -1, wake = -1, out = -1;
    if (sh->interactive && !sh->closed) {
        if (mr_pending(&sh->rd))
            timeout_ms = 0;
        gui = k;
        pfd[k].fd = sh->gui_in;
        pfd[k++].events = POLLIN;
    }
    if (sh->wake_fd >= 0) {
        wake = k;
        pfd[k].fd = sh->wake_fd;
        pfd[k++].events = POLLIN;
    }
    if (sh->out_r >= 0) {
        out = k;
        pfd[k].fd = sh->out_r;
        pfd[k++].events = POLLIN;
    }
    int base = k;
    for (int i = 0; i < nfds && k < (int)ARRAY_LEN(pfd); i++) {
        pfd[k].fd = fds[i];
        pfd[k++].events = POLLIN;
    }
    int r = poll(pfd, (nfds_t)k, timeout_ms);
    if (r < 0) {
        if (revents)
            for (int i = 0; i < nfds; i++)
                revents[i] = 0;
        return 0;
    }
    (void)gui;
    if (wake >= 0 && (pfd[wake].revents & POLLIN))
        wake_drain(sh->wake_fd);
    if (out >= 0 && (pfd[out].revents & (POLLIN | POLLHUP)))
        sh_forward_output(sh);
    if (revents)
        for (int i = 0; i < nfds; i++)
            revents[i] = pfd[base + i].revents;
    return r;
}

/* ----------------------------------------------------------- children */

pid_t shell_fork(Shell *sh)
{
    pid_t pid = fork();
    if (pid == 0) {
        /* Terminator ignores the job-control signals; its children must
         * not inherit that. */
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        wake_reset_child();
        sh->interactive = 0;
        sh->fd_in = 0;
        sh->fd_out = 1;
        sh->fd_err = 2;
    }
    return pid;
}

void shell_enter_subshell(Shell *sh)
{
    /* Only 0/1/2 are needed from here on.  Closing the rest matters: in
     * thread mode this process image also holds other tabs' pipes. */
    close_from(3);
    sh->interactive = 0;
    sh->gui_in = sh->gui_out = -1;
    sh->out_r = sh->out_w = -1;
    sh->wake_fd = -1;
    sh->wake_slot = -1;
    for (Job *j = sh->jobs; j; j = j->next)
        j->stdin_w = -1; /* already closed above */
    while (sh->jobs)
        job_remove(sh, sh->jobs);
    sh->fd_in = 0;
    sh->fd_out = 1;
    sh->fd_err = 2;
}

_Noreturn void shell_exit_child(Shell *sh, int status)
{
    (void)sh;
    _exit(status & 0xff);
}

int shell_run_string(Shell *sh, const char *src, const char *name)
{
    Node *root = NULL;
    char err[256];
    int line = 0;
    int r = parse_program(src, &root, err, sizeof err, &line);
    if (r != PARSE_OK) {
        if (name)
            sh_errorf(sh, "%s: line %d: %s", name, line, err);
        else
            sh_errorf(sh, "%s", err);
        sh->last_status = 2;
        return 2;
    }
    int st = exec_node(sh, root);
    sh->last_status = st;
    node_free(root);
    if (sh->exit_requested)
        st = sh->exit_code;
    return st;
}

/* ---------------------------------------------------- interactive loop */

static int is_blank_line(const char *s)
{
    for (; *s; s++)
        if (*s != ' ' && *s != '\t' && *s != '\n')
            return 0;
    return 1;
}

static void handle_line(Shell *sh, char *line)
{
    buf_puts(&sh->pending, line);
    free(line);

    Node *root = NULL;
    char err[256];
    int eline = 0;
    int r = parse_program(sh->pending.s, &root, err, sizeof err, &eline);
    if (r == PARSE_INCOMPLETE) {
        buf_putc(&sh->pending, '\n'); /* keep reading with the "> " prompt */
        return;
    }
    char *text = buf_detach(&sh->pending);
    if (!is_blank_line(text))
        hist_add(text);
    if (r == PARSE_ERROR) {
        sh_errorf(sh, "%s", err);
        sh->last_status = 2;
    } else {
        ed_begin(sh, ED_JOB, "");
        sh->interrupted = 0;
        sh->last_status = exec_node(sh, root);
        node_free(root);
    }
    free(text);
}

/* */
static char *get_shell_prompt(Shell *sh)
{
    char cwd[4096];

    if (!getcwd(cwd, sizeof cwd))
        snprintf(cwd, sizeof cwd, "?");

    char *path = collapse_home(cwd);

    char *user = user_name();

    char *prompt = xasprintf("%s:%s$ ", user, path);

    free(user);
    free(path);

    return prompt;
}
/* */
void shell_interactive_loop(Shell *sh)
{
    sh_send_title(sh);
    while (!sh->exit_requested && !sh->closed) {
        sh_forward_output(sh);
        jobs_notify(sh);
        sh->interrupted = 0;
        // ed_begin(sh, ED_PROMPT, sh->pending.len ? "> " : sh->main_prompt);

        // ----------
        char *prompt = get_shell_prompt(sh);

        ed_begin(sh, ED_PROMPT, sh->pending.len ? "> " : prompt);

        free(prompt);
        // ----------
        int ev;
        for (;;) {
            ev = shell_poll_input(sh);
            if (ev != EV_NONE)
                break;
            shell_wait(sh, NULL, 0, NULL, -1);
            jobs_notify(sh); /* "[1]+ Done ..." shows up right away */
        }
        switch (ev) {
        case EV_LINE: {
            char *line = sh->ready_line;
            sh->ready_line = NULL;
            handle_line(sh, line);
            break;
        }
        case EV_INTR:
            buf_clear(&sh->pending);
            sh->last_status = 130;
            break;
        case EV_EOF:
            if (sh->pending.len) {
                sh_errorf(sh, "syntax error: unexpected end of file");
                buf_clear(&sh->pending);
            } else if (jobs_stopped_count(sh) > 0 && !sh->warned_jobs) {
                sh_out(sh, "exit\nThere are stopped jobs.\n");
                sh->warned_jobs = 1;
            } else {
                sh_out(sh, "exit\n");
                sh->exit_requested = 1;
                sh->exit_code = sh->last_status;
            }
            break;
        case EV_CLOSE:
        default:
            break;
        }
    }
    ed_hide(sh);
    jobs_kill_all(sh);
    sh_forward_output(sh);
    if (!sh->closed)
        msg_send_int(sh->gui_out, M_EXIT, sh->exit_code, NULL, 0);
}
