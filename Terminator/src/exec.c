/*
 * exec.c - running parsed commands: lists, && / ||, if, pipelines, simple
 * commands, redirections, external programs and bash scripts.
 *
 * External commands always run in a child created with fork() and started
 * with execve() (PATH is searched by the shell itself so it can recognise
 * shell scripts and interpret them instead of handing them to /bin/bash).
 */
#include "shell.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct XRedir {
    RedirType type;
    int fd;
    char *target; /* expanded */
    struct XRedir *next;
} XRedir;

typedef struct {
    StrVec argv;
    StrVec assigns; /* expanded NAME=value */
    XRedir *redirs;
    char **raw;     /* unexpanded words (multiWatch needs them) */
} Cmd;

int wait_pid_blocking(pid_t pid)
{
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR)
            return 0;
    }
    return st;
}

int status_to_code(int st)
{
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    if (WIFSIGNALED(st))
        return 128 + WTERMSIG(st);
    if (WIFSTOPPED(st))
        return 128 + WSTOPSIG(st);
    return 0;
}

/* ------------------------------------------------------------ prepare */

static void cmd_free(Cmd *c)
{
    sv_free(&c->argv);
    sv_free(&c->assigns);
    for (XRedir *r = c->redirs, *nx; r; r = nx) {
        nx = r->next;
        free(r->target);
        free(r);
    }
    c->redirs = NULL;
}

static void prepare_cmd(Shell *sh, Node *n, Cmd *c)
{
    sv_init(&c->argv);
    sv_init(&c->assigns);
    c->redirs = NULL;
    c->raw = n->words;
    for (int i = 0; i < n->nassigns; i++) {
        size_t nl = assignment_name_len(n->assigns[i]);
        char *val = expand_str(sh, n->assigns[i] + nl + 1);
        sv_push(&c->assigns, xasprintf("%.*s=%s", (int)nl, n->assigns[i], val));
        free(val);
    }
    if (n->nwords > 0 && strcmp(n->words[0], "multiWatch") == 0) {
        /* multiWatch parses its own ["cmd", ...] list from the raw text */
        for (int i = 0; i < n->nwords; i++)
            sv_pushdup(&c->argv, n->words[i]);
    } else {
        for (int i = 0; i < n->nwords; i++)
            expand_word(sh, n->words[i], &c->argv, 0);
    }
    XRedir **tail = &c->redirs;
    for (Redir *r = n->redirs; r; r = r->next) {
        XRedir *x = xcalloc(1, sizeof *x);
        x->type = r->type;
        x->fd = r->fd;
        x->target = expand_str(sh, r->target);
        *tail = x;
        tail = &x->next;
    }
}

static int redirects_stdout(Cmd *c)
{
    for (XRedir *r = c->redirs; r; r = r->next)
        if (r->fd == 1 || r->type == R_BOTH)
            return 1;
    return 0;
}

/* `ls` writing to the terminal: ask for bash-like columns (stdout is a pipe,
 * so ls would otherwise print one name per line). */
static void tweak_for_terminal(Shell *sh, Cmd *c)
{
    (void)sh;
    if (c->argv.n == 0 || strcmp(c->argv.v[0], "ls") != 0 || redirects_stdout(c))
        return;
    StrVec nv;
    sv_init(&nv);
    sv_pushdup(&nv, "ls");
    sv_pushdup(&nv, "-C");
    for (int i = 1; i < c->argv.n; i++)
        sv_pushdup(&nv, c->argv.v[i]);
    sv_free(&c->argv);
    c->argv = nv;
}

/* -------------------------------------------------------- redirection */

static int is_number(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9')
            return 0;
    return 1;
}

static int open_target(Shell *sh, XRedir *r)
{
    int flags;
    switch (r->type) {
    case R_IN:
        flags = O_RDONLY;
        break;
    case R_APPEND:
        flags = O_WRONLY | O_CREAT | O_APPEND;
        break;
    default:
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        break;
    }
    if (!*r->target) {
        sh_errorf(sh, "ambiguous redirect");
        return -1;
    }
    /* Each redirection opens the file anew, so every job gets its own
     * open file description and file offset: two background jobs reading
     * the same file never disturb each other. */
    int fd = open(r->target, flags | O_CLOEXEC, 0666);
    if (fd < 0)
        sh_errorf(sh, "%s: %s", r->target, strerror(errno));
    return fd;
}

/* In a forked child: wire the descriptors for real. */
static int apply_redirs_child(Shell *sh, XRedir *r)
{
    for (; r; r = r->next) {
        if (r->type == R_DUP && (is_number(r->target) || strcmp(r->target, "-") == 0)) {
            if (strcmp(r->target, "-") == 0)
                close(r->fd);
            else if (dup2(atoi(r->target), r->fd) < 0) {
                sh_errorf(sh, "%s: bad file descriptor", r->target);
                return -1;
            }
            continue;
        }
        XRedir tmp = *r;
        if (tmp.type == R_DUP)
            tmp.type = R_BOTH; /* ">&file" means "&>file" */
        int fd = open_target(sh, &tmp);
        if (fd < 0)
            return -1;
        if (tmp.type == R_BOTH) {
            dup2(fd, 1);
            dup2(fd, 2);
        } else {
            dup2(fd, r->fd);
        }
        if (fd != r->fd && fd > 2)
            close(fd);
    }
    return 0;
}

/* For a builtin running inside the shell: redirect sh->fd_in/out/err. */
static int apply_redirs_shell(Shell *sh, XRedir *r, int *opened, int *nopen)
{
    for (; r; r = r->next) {
        if (r->type == R_DUP && is_number(r->target)) {
            int src = atoi(r->target);
            int val = src == 0 ? sh->fd_in : src == 1 ? sh->fd_out : src == 2 ? sh->fd_err : src;
            if (r->fd == 1)
                sh->fd_out = val;
            else if (r->fd == 2)
                sh->fd_err = val;
            else if (r->fd == 0)
                sh->fd_in = val;
            continue;
        }
        if (r->type == R_DUP && strcmp(r->target, "-") == 0)
            continue;
        XRedir tmp = *r;
        if (tmp.type == R_DUP)
            tmp.type = R_BOTH;
        int fd = open_target(sh, &tmp);
        if (fd < 0)
            return -1;
        opened[(*nopen)++] = fd;
        if (tmp.type == R_BOTH) {
            sh->fd_out = fd;
            sh->fd_err = fd;
        } else if (r->fd == 0) {
            sh->fd_in = fd;
        } else if (r->fd == 1) {
            sh->fd_out = fd;
        } else if (r->fd == 2) {
            sh->fd_err = fd;
        }
        if (*nopen >= 30)
            break;
    }
    return 0;
}

static int run_builtin_in_shell(Shell *sh, BuiltinFn fn, Cmd *c)
{
    int save_in = sh->fd_in, save_out = sh->fd_out, save_err = sh->fd_err;
    int opened[32], nopen = 0;
    int status;
    if (apply_redirs_shell(sh, c->redirs, opened, &nopen) < 0) {
        status = 1;
    } else {
        if (sh->interactive)
            sh_forward_output(sh);
        status = fn(sh, c->argv.n, c->argv.v, c->raw);
    }
    for (int i = 0; i < nopen; i++)
        close(opened[i]);
    sh->fd_in = save_in;
    sh->fd_out = save_out;
    sh->fd_err = save_err;
    return status;
}

/* -------------------------------------------------- external programs */

static char *resolve_path(Shell *sh, const char *name)
{
    if (strchr(name, '/'))
        return access(name, F_OK) == 0 ? xstrdup(name) : NULL;
    const char *path = var_get(sh, "PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin:/bin";
    const char *p = path;
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        char *cand = len ? xasprintf("%.*s/%s", (int)len, p, name) : xasprintf("./%s", name);
        struct stat st;
        if (stat(cand, &st) == 0 && S_ISREG(st.st_mode) && access(cand, X_OK) == 0)
            return cand;
        free(cand);
        if (!colon)
            break;
        p = colon + 1;
    }
    return NULL;
}

/* 1: a bash/sh script we interpret ourselves; 0: a binary or another
 * interpreter's script (let the kernel run it); -1: unknown. */
static int is_shell_script(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    char head[256];
    ssize_t n = read_retry(fd, head, sizeof head - 1);
    close(fd);
    if (n < 0)
        return 0;
    head[n] = '\0';
    if (n >= 4 && memcmp(head, "\x7f" "ELF", 4) == 0)
        return 0;
    if (n >= 2 && head[0] == '#' && head[1] == '!') {
        char *p = head + 2;
        while (*p == ' ' || *p == '\t')
            p++;
        char *end = p;
        while (*end && *end != ' ' && *end != '\t' && *end != '\n')
            end++;
        char interp[256];
        snprintf(interp, sizeof interp, "%.*s", (int)(end - p), p);
        char *base = strrchr(interp, '/');
        base = base ? base + 1 : interp;
        if (strcmp(base, "env") == 0) {
            p = end;
            while (*p == ' ' || *p == '\t')
                p++;
            end = p;
            while (*end && *end != ' ' && *end != '\t' && *end != '\n')
                end++;
            snprintf(interp, sizeof interp, "%.*s", (int)(end - p), p);
            base = interp;
        }
        return strcmp(base, "bash") == 0 || strcmp(base, "sh") == 0 || strcmp(base, "dash") == 0;
    }
    return -1;
}

static void set_script_args(Shell *sh, const char *arg0, char **args, int nargs)
{
    sh->arg0 = xstrdup(arg0);
    sh->posv = xcalloc((size_t)nargs + 1, sizeof(char *));
    for (int i = 0; i < nargs; i++)
        sh->posv[i] = xstrdup(args[i]);
    sh->posc = nargs;
}

static int count_args(char **v)
{
    int n = 0;
    while (v && v[n])
        n++;
    return n;
}

_Noreturn void exec_external(Shell *sh, char **argv, StrVec *assigns)
{
    const char *name = argv[0];
    for (int i = 0; assigns && i < assigns->n; i++) {
        /* prefix assignments are visible to an interpreted script too */
        char *eq = strchr(assigns->v[i], '=');
        char *nm = xstrndup(assigns->v[i], (size_t)(eq - assigns->v[i]));
        var_set(sh, nm, eq + 1, 1);
        free(nm);
    }

    /* "bash script.sh args" / "sh -c 'cmd'" are interpreted, not exec'd */
    if ((strcmp(name, "bash") == 0 || strcmp(name, "sh") == 0) && argv[1]) {
        if (strcmp(argv[1], "-c") == 0 && argv[2]) {
            shell_enter_subshell(sh);
            set_script_args(sh, argv[3] ? argv[3] : name, argv[3] ? argv + 4 : argv + 3,
                            argv[3] ? count_args(argv + 4) : 0);
            subshell_run_string(sh, argv[2]);
        }
        if (argv[1][0] != '-') {
            shell_enter_subshell(sh);
            shell_exit_child(sh, run_script_file(sh, argv[1], argv + 2, count_args(argv + 2), 0));
        }
    }

    char *path = resolve_path(sh, name);
    if (!path) {
        if (strchr(name, '/'))
            sh_errorf(sh, "%s: No such file or directory", name);
        else
            sh_errorf(sh, "%s: command not found", name);
        _exit(127);
    }
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        sh_errorf(sh, "%s: Is a directory", name);
        _exit(126);
    }
    if (access(path, X_OK) != 0) {
        sh_errorf(sh, "%s: Permission denied", name);
        _exit(126);
    }
    if (is_shell_script(path) == 1) {
        shell_enter_subshell(sh);
        shell_exit_child(sh, run_script_file(sh, path, argv + 1, count_args(argv + 1), 0));
    }
    char **envp = vars_build_env(sh, NULL);
    execve(path, argv, envp);
    int err = errno;
    if (err == ENOEXEC) {
        /* a text file without "#!": run it as a shell script, like bash */
        shell_enter_subshell(sh);
        shell_exit_child(sh, run_script_file(sh, path, argv + 1, count_args(argv + 1), 0));
    }
    sh_errorf(sh, "%s: %s", name, strerror(err));
    _exit(err == ENOENT ? 127 : 126);
}

/* Child side of a simple command whose words are already expanded. */
static _Noreturn void child_run_cmd(Shell *sh, Cmd *c)
{
    if (apply_redirs_child(sh, c->redirs) < 0)
        _exit(1);
    if (c->argv.n == 0)
        _exit(0);
    BuiltinFn fn = builtin_lookup(c->argv.v[0]);
    if (fn) {
        shell_enter_subshell(sh);
        shell_exit_child(sh, fn(sh, c->argv.n, c->argv.v, c->raw));
    }
    exec_external(sh, c->argv.v, &c->assigns);
}

/* Child side of one member of a pipeline (or of a background job). */
static _Noreturn void child_run_node(Shell *sh, Node *n, int to_term)
{
    shell_enter_subshell(sh);
    if (n->type == N_SIMPLE) {
        Cmd c;
        prepare_cmd(sh, n, &c);
        if (to_term)
            tweak_for_terminal(sh, &c);
        child_run_cmd(sh, &c);
    }
    int st = exec_node(sh, n);
    if (sh->exit_requested)
        st = sh->exit_code;
    shell_exit_child(sh, st);
}

_Noreturn void subshell_run_string(Shell *sh, const char *src)
{
    Node *root = NULL;
    char err[256];
    int line;
    int r = parse_program(src, &root, err, sizeof err, &line);
    if (r != PARSE_OK) {
        sh_errorf(sh, "%s", err);
        _exit(2);
    }
    /* A single plain external command: exec it directly in this process
     * instead of forking once more (multiWatch: "one process each"). */
    if (root->nitems == 1 && !root->bg[0] && root->items[0]->type == N_SIMPLE) {
        Node *n = root->items[0];
        Cmd c;
        prepare_cmd(sh, n, &c);
        if (c.argv.n > 0 && !builtin_lookup(c.argv.v[0]))
            child_run_cmd(sh, &c);
        cmd_free(&c);
    }
    int st = exec_node(sh, root);
    if (sh->exit_requested)
        st = sh->exit_code;
    shell_exit_child(sh, st);
}

/* ---------------------------------------------------------- pipelines */

static int exec_pipeline(Shell *sh, Node **cmds, int n, int negate, int bg, const char *text)
{
    int interactive = sh->interactive;
    Job *job = interactive ? job_new(sh, text) : NULL;
    int jin[2] = { -1, -1 };
    if (interactive && make_pipe(jin, 1) == 0) {
        set_nonblock(jin[1], 1);
        job->stdin_w = jin[1];
    }
    pid_t pgid = 0, last = 0;
    pid_t *pids = xcalloc((size_t)n, sizeof(pid_t));
    int prev_r = -1;
    int failed = 0;

    for (int i = 0; i < n; i++) {
        int p[2] = { -1, -1 };
        if (i < n - 1 && make_pipe(p, 1) < 0) {
            sh_errorf(sh, "pipe: %s", strerror(errno));
            failed = 1;
            break;
        }
        int to_term = interactive && i == n - 1;
        pid_t pid = shell_fork(sh);
        if (pid < 0) {
            sh_errorf(sh, "fork: %s", strerror(errno));
            if (p[0] >= 0) {
                close(p[0]);
                close(p[1]);
            }
            failed = 1;
            break;
        }
        if (pid == 0) {
            if (interactive)
                setpgid(0, pgid);
            if (i == 0) {
                if (interactive && jin[0] >= 0) {
                    dup2(jin[0], 0);
                } else if (bg) {
                    int dn = open("/dev/null", O_RDONLY);
                    if (dn >= 0) {
                        dup2(dn, 0);
                        close(dn);
                    }
                }
            } else {
                dup2(prev_r, 0);
            }
            if (i < n - 1)
                dup2(p[1], 1);
            else if (interactive)
                dup2(sh->out_w, 1);
            if (interactive)
                dup2(sh->out_w, 2);
            child_run_node(sh, cmds[i], to_term);
        }
        if (interactive) {
            if (!pgid)
                pgid = pid;
            setpgid(pid, pgid);
            job_add_pid(job, pid);
        }
        pids[i] = pid;
        last = pid;
        if (prev_r >= 0)
            close(prev_r);
        if (p[1] >= 0)
            close(p[1]);
        prev_r = p[0];
    }
    if (prev_r >= 0)
        close(prev_r);
    if (jin[0] >= 0)
        close(jin[0]);

    int status = 0;
    if (interactive) {
        job->pgid = pgid;
        if (job->npids == 0) {
            job_remove(sh, job);
            free(pids);
            return 1;
        }
        if (bg) {
            job->background = 1;
            job_set_current(sh, job);
            sh->last_bg = last;
            sh_printf(sh, "[%d] %ld\n", job->id, (long)pgid);
            free(pids);
            return 0;
        }
        status = job_wait_fg(sh, job);
    } else {
        if (bg) {
            sh->last_bg = last;
            free(pids);
            return 0;
        }
        for (int i = 0; i < n; i++) {
            if (!pids[i])
                continue;
            int st = wait_pid_blocking(pids[i]);
            if (pids[i] == last)
                status = status_to_code(st);
        }
    }
    free(pids);
    if (failed && status == 0)
        status = 1;
    if (negate)
        status = status == 0 ? 1 : 0;
    return status;
}

static int exec_simple(Shell *sh, Node *n)
{
    Cmd c;
    sh->subst_ran = 0;
    prepare_cmd(sh, n, &c);
    int subst_status = sh->subst_ran ? sh->subst_status : 0;
    int status;

    if (sh->interrupted) {
        cmd_free(&c);
        return 130;
    }
    if (c.argv.n == 0) {
        /* assignments and/or redirections only */
        for (int i = 0; i < c.assigns.n; i++) {
            char *eq = strchr(c.assigns.v[i], '=');
            char *name = xstrndup(c.assigns.v[i], (size_t)(eq - c.assigns.v[i]));
            var_set(sh, name, eq + 1, 0);
            free(name);
        }
        int opened[32], nopen = 0;
        int save_in = sh->fd_in, save_out = sh->fd_out, save_err = sh->fd_err;
        status = apply_redirs_shell(sh, c.redirs, opened, &nopen) < 0 ? 1 : subst_status;
        for (int i = 0; i < nopen; i++)
            close(opened[i]);
        sh->fd_in = save_in;
        sh->fd_out = save_out;
        sh->fd_err = save_err;
        cmd_free(&c);
        return status;
    }

    BuiltinFn fn = builtin_lookup(c.argv.v[0]);
    if (fn) {
        status = run_builtin_in_shell(sh, fn, &c);
        cmd_free(&c);
        return status;
    }

    if (sh->interactive)
        tweak_for_terminal(sh, &c);

    if (!sh->interactive) {
        pid_t pid = shell_fork(sh);
        if (pid < 0) {
            sh_errorf(sh, "fork: %s", strerror(errno));
            cmd_free(&c);
            return 1;
        }
        if (pid == 0)
            child_run_cmd(sh, &c);
        status = status_to_code(wait_pid_blocking(pid));
        cmd_free(&c);
        return status;
    }

    /* interactive: the command becomes a foreground job */
    Job *job = job_new(sh, n->text);
    int jin[2] = { -1, -1 };
    if (make_pipe(jin, 1) == 0) {
        set_nonblock(jin[1], 1);
        job->stdin_w = jin[1];
    }
    pid_t pid = shell_fork(sh);
    if (pid < 0) {
        sh_errorf(sh, "fork: %s", strerror(errno));
        if (jin[0] >= 0)
            close(jin[0]);
        job_remove(sh, job);
        cmd_free(&c);
        return 1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        if (jin[0] >= 0)
            dup2(jin[0], 0);
        dup2(sh->out_w, 1);
        dup2(sh->out_w, 2);
        child_run_cmd(sh, &c);
    }
    setpgid(pid, pid);
    if (jin[0] >= 0)
        close(jin[0]);
    job->pgid = pid;
    job_add_pid(job, pid);
    status = job_wait_fg(sh, job);
    cmd_free(&c);
    return status;
}

static int exec_background(Shell *sh, Node *n)
{
    if (n->type == N_PIPE && !n->negate)
        return exec_pipeline(sh, n->cmds, n->ncmds, 0, 1, n->text);
    Node *one[1] = { n };
    return exec_pipeline(sh, one, 1, 0, 1, n->text);
}

int exec_node(Shell *sh, Node *n)
{
    if (!n)
        return 0;
    int status = 0;
    switch (n->type) {
    case N_LIST:
        for (int i = 0; i < n->nitems; i++) {
            if (sh->exit_requested || sh->interrupted)
                break;
            status = n->bg[i] ? exec_background(sh, n->items[i]) : exec_node(sh, n->items[i]);
            sh->last_status = status;
        }
        return status;
    case N_ANDOR:
        status = exec_node(sh, n->parts[0]);
        sh->last_status = status;
        for (int k = 1; k < n->nparts; k++) {
            if (sh->exit_requested || sh->interrupted)
                break;
            int op = n->ops[k - 1];
            if ((op == 1 && status == 0) || (op == 2 && status != 0)) {
                status = exec_node(sh, n->parts[k]);
                sh->last_status = status;
            }
        }
        return status;
    case N_PIPE:
        return exec_pipeline(sh, n->cmds, n->ncmds, n->negate, 0, n->text);
    case N_SIMPLE:
        return exec_simple(sh, n);
    case N_IF:
        for (int k = 0; k < n->nconds; k++) {
            int c = exec_node(sh, n->conds[k]);
            sh->last_status = c;
            if (sh->exit_requested || sh->interrupted)
                return c;
            if (c == 0)
                return exec_node(sh, n->bodies[k]);
        }
        return n->else_body ? exec_node(sh, n->else_body) : 0;
    }
    return status;
}

/* ------------------------------------------------------------ scripts */

int run_script_file(Shell *sh, const char *path, char **args, int nargs, int in_place)
{
    size_t len;
    char *src = read_file(path, &len);
    if (!src) {
        sh_errorf(sh, "%s: %s", path, strerror(errno));
        return 127;
    }
    if (strlen(src) != len) {
        sh_errorf(sh, "%s: cannot execute binary file", path);
        free(src);
        return 126;
    }
    Node *root = NULL;
    char err[256];
    int line = 0;
    int r = parse_program(src, &root, err, sizeof err, &line);
    if (r != PARSE_OK) {
        /* the whole script is parsed first: nothing runs on a syntax error */
        sh_errorf(sh, "%s: line %d: %s", path, line, err);
        free(src);
        return 2;
    }

    char *save_arg0 = sh->arg0;
    char **save_posv = sh->posv;
    int save_posc = sh->posc;
    int replaced = 0;
    if (!in_place) {
        set_script_args(sh, path, args, nargs);
        replaced = 1;
    } else if (nargs > 0) {
        sh->posv = xcalloc((size_t)nargs + 1, sizeof(char *));
        for (int i = 0; i < nargs; i++)
            sh->posv[i] = xstrdup(args[i]);
        sh->posc = nargs;
        sh->arg0 = xstrdup(save_arg0 ? save_arg0 : "terminator");
        replaced = 1;
    }

    sh->source_depth++;
    int status = exec_node(sh, root);
    sh->source_depth--;
    if (sh->exit_requested)
        status = sh->exit_code;

    if (replaced && in_place) {
        for (int i = 0; i < sh->posc; i++)
            free(sh->posv[i]);
        free(sh->posv);
        free(sh->arg0);
        sh->arg0 = save_arg0;
        sh->posv = save_posv;
        sh->posc = save_posc;
    }
    node_free(root);
    free(src);
    return status;
}
