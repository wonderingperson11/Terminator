/*
 * builtins.c - commands executed by the shell itself.
 *
 * Anything that changes the shell's own state (cd, exit, variables, jobs,
 * source) must be a builtin: a child process could only change its own
 * copy.  Builtins write through sh_* helpers so their output reaches the
 * GUI, a redirected file or a pipe as appropriate.
 */
#include "keys.h"
#include "shell.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ----------------------------------------------------------------- cd */
static int bi_cd(Shell *sh, int argc, char **argv, char **raw)
{
    (void)raw;
    const char *target = argc > 1 ? argv[1] : var_get(sh, "HOME");
    int print = 0;
    if (!target) {
        sh_errorf(sh, "cd: HOME not set");
        return 1;
    }
    if (strcmp(target, "-") == 0) {
        target = var_get(sh, "OLDPWD");
        if (!target) {
            sh_errorf(sh, "cd: OLDPWD not set");
            return 1;
        }
        print = 1;
    }
    char old[4096];
    if (!getcwd(old, sizeof old))
        old[0] = '\0';
    if (chdir(target) != 0) {
        sh_errorf(sh, "cd: %s: %s", target, strerror(errno));
        return 1;
    }
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd))
        var_set(sh, "PWD", cwd, 1);
    if (old[0])
        var_set(sh, "OLDPWD", old, 1);
    if (print)
        sh_printf(sh, "%s\n", cwd);
    sh_send_title(sh);
    return 0;
}

static int bi_pwd(Shell *sh, int argc, char **argv, char **raw)
{
    (void)argc, (void)argv, (void)raw;
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) {
        sh_errorf(sh, "pwd: %s", strerror(errno));
        return 1;
    }
    sh_printf(sh, "%s\n", cwd);
    return 0;
}

/* --------------------------------------------------------------- echo */
static int bi_echo(Shell *sh, int argc, char **argv, char **raw)
{
    (void)raw;
    int newline = 1, escapes = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o = argv[i] + 1;
        if (strspn(o, "neE") != strlen(o))
            break;
        for (; *o; o++) {
            if (*o == 'n')
                newline = 0;
            else if (*o == 'e')
                escapes = 1;
            else
                escapes = 0;
        }
    }
    Buf b;
    buf_init(&b);
    for (int k = i; k < argc; k++) {
        if (k > i)
            buf_putc(&b, ' ');
        if (!escapes) {
            buf_puts(&b, argv[k]);
            continue;
        }
        for (const char *s = argv[k]; *s; s++) {
            if (*s != '\\' || !s[1]) {
                buf_putc(&b, *s);
                continue;
            }
            s++;
            switch (*s) {
            case 'n': buf_putc(&b, '\n'); break;
            case 't': buf_putc(&b, '\t'); break;
            case 'r': buf_putc(&b, '\r'); break;
            case 'a': buf_putc(&b, '\a'); break;
            case 'b': buf_putc(&b, '\b'); break;
            case 'v': buf_putc(&b, '\v'); break;
            case 'f': buf_putc(&b, '\f'); break;
            case 'e': buf_putc(&b, '\033'); break;
            case '\\': buf_putc(&b, '\\'); break;
            case 'c':
                sh_write(sh, sh->fd_out, b.s ? b.s : "", b.len);
                buf_free(&b);
                return 0;
            case '0': {
                int v = 0, d = 0;
                while (d < 3 && s[1] >= '0' && s[1] <= '7') {
                    v = v * 8 + (*++s - '0');
                    d++;
                }
                buf_putc(&b, (char)v);
                break;
            }
            default:
                buf_putc(&b, '\\');
                buf_putc(&b, *s);
            }
        }
    }
    if (newline)
        buf_putc(&b, '\n');
    sh_write(sh, sh->fd_out, b.s ? b.s : "", b.len);
    buf_free(&b);
    return 0;
}

/* --------------------------------------------------------------- exit */
static int bi_exit(Shell *sh, int argc, char **argv, char **raw)
{
    (void)raw;
    int code = sh->last_status;
    if (argc > 1) {
        char *end;
        long v = strtol(argv[1], &end, 10);
        if (*end) {
            sh_errorf(sh, "exit: %s: numeric argument required", argv[1]);
            code = 2;
        } else {
            code = (int)(v & 0xff);
        }
    }
    if (sh->interactive && sh->source_depth == 0 && !sh->warned_jobs && jobs_stopped_count(sh) > 0) {
        sh_out(sh, "There are stopped jobs.\n");
        sh->warned_jobs = 1;
        return 1;
    }
    if (sh->interactive)
        sh_out(sh, "exit\n");
    sh->exit_requested = 1;
    sh->exit_code = code;
    return code;
}

/* ------------------------------------------------------------ history */
static int bi_history(Shell *sh, int argc, char **argv, char **raw)
{
    (void)raw;
    if (argc > 1 && strcmp(argv[1], "-c") == 0) {
        hist_clear();
        return 0;
    }
    int n = 1000;
    if (argc > 1) {
        n = atoi(argv[1]);
        if (n <= 0) {
            sh_errorf(sh, "history: %s: numeric argument required", argv[1]);
            return 1;
        }
    }
    hist_show(sh, n);
    return 0;
}

/* --------------------------------------------------------------- jobs */
static int bi_jobs(Shell *sh, int argc, char **argv, char **raw)
{
    (void)argc, (void)argv, (void)raw;
    jobs_reap(sh);
    for (Job *j = sh->jobs; j; j = j->next)
        job_print(sh, j, 1);
    for (Job *j = sh->jobs, *nx; j; j = nx) {
        nx = j->next;
        if (j->state == JOB_DONE)
            job_remove(sh, j);
    }
    return 0;
}

static void job_continue(Job *j)
{
    for (int i = 0; i < j->npids; i++)
        j->stopped[i] = 0;
    j->state = JOB_RUNNING;
    kill(-j->pgid, SIGCONT);
}

static int bi_fg(Shell *sh, int argc, char **argv, char **raw)
{
    (void)raw;
    if (!sh->interactive) {
        sh_errorf(sh, "fg: no job control");
        return 1;
    }
    int err;
    jobs_reap(sh);
    Job *j = job_find(sh, argc > 1 ? argv[1] : NULL, &err);
    if (!j) {
        sh_errorf(sh, "fg: %s: no such job", argc > 1 ? argv[1] : "current");
        return 1;
    }
    sh_printf(sh, "%s\n", j->cmd);
    job_set_current(sh, j);
    job_continue(j);
    return job_wait_fg(sh, j);
}

static int bi_bg(Shell *sh, int argc, char **argv, char **raw)
{
    (void)raw;
    if (!sh->interactive) {
        sh_errorf(sh, "bg: no job control");
        return 1;
    }
    jobs_reap(sh);
    int status = 0;
    for (int k = 1; k < (argc > 1 ? argc : 2); k++) {
        int err;
        Job *j = job_find(sh, argc > 1 ? argv[k] : NULL, &err);
        if (!j) {
            sh_errorf(sh, "bg: %s: no such job", argc > 1 ? argv[k] : "current");
            status = 1;
            continue;
        }
        if (j->state == JOB_RUNNING) {
            sh_errorf(sh, "bg: job %d already in background", j->id);
            continue;
        }
        j->background = 1;
        job_continue(j);
        job_print(sh, j, 0);
    }
    return status;
}

/* --------------------------------------------------------------- kill */
static const struct {
    const char *name;
    int sig;
} signames[] = {
    { "HUP", SIGHUP },   { "INT", SIGINT },   { "QUIT", SIGQUIT }, { "KILL", SIGKILL },
    { "TERM", SIGTERM }, { "STOP", SIGSTOP }, { "TSTP", SIGTSTP }, { "CONT", SIGCONT },
    { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 }, { "ALRM", SIGALRM }, { "PIPE", SIGPIPE },
    { "CHLD", SIGCHLD }, { "SEGV", SIGSEGV }, { "ABRT", SIGABRT }, { "TTIN", SIGTTIN },
    { "TTOU", SIGTTOU },
};

static int parse_signal(const char *s)
{
    if (s[0] >= '0' && s[0] <= '9')
        return atoi(s);
    if (strncasecmp(s, "SIG", 3) == 0)
        s += 3;
    for (size_t i = 0; i < ARRAY_LEN(signames); i++)
        if (strcasecmp(s, signames[i].name) == 0)
            return signames[i].sig;
    return -1;
}

static int bi_kill(Shell *sh, int argc, char **argv, char **raw)
{
    (void)raw;
    int sig = SIGTERM, i = 1;
    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        for (size_t k = 0; k < ARRAY_LEN(signames); k++)
            sh_printf(sh, "%2d) SIG%s\n", signames[k].sig, signames[k].name);
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "-s") == 0) {
        sig = parse_signal(argv[2]);
        i = 3;
    } else if (argc > 1 && argv[1][0] == '-' && argv[1][1]) {
        sig = parse_signal(argv[1] + 1);
        i = 2;
    }
    if (sig < 0) {
        sh_errorf(sh, "kill: invalid signal specification");
        return 1;
    }
    if (i >= argc) {
        sh_errorf(sh, "kill: usage: kill [-s sigspec | -signum] pid | %%job ...");
        return 2;
    }
    int status = 0;
    for (; i < argc; i++) {
        pid_t target;
        if (argv[i][0] == '%') {
            int err;
            Job *j = job_find(sh, argv[i], &err);
            if (!j) {
                sh_errorf(sh, "kill: %s: no such job", argv[i]);
                status = 1;
                continue;
            }
            target = -j->pgid;
        } else {
            char *end;
            target = (pid_t)strtol(argv[i], &end, 10);
            if (*end || !*argv[i]) {
                sh_errorf(sh, "kill: %s: arguments must be process or job IDs", argv[i]);
                status = 1;
                continue;
            }
        }
        if (kill(target, sig) < 0) {
            sh_errorf(sh, "kill: (%s) - %s", argv[i], strerror(errno));
            status = 1;
        } else if (target < 0 && (sig == SIGTERM || sig == SIGKILL || sig == SIGHUP || sig == SIGINT)) {
            kill(target, SIGCONT); /* a stopped job must wake up to die */
        }
    }
    return status;
}

static int bi_wait(Shell *sh, int argc, char **argv, char **raw)
{
    (void)argc, (void)argv, (void)raw;
    if (!sh->interactive) {
        while (wait(NULL) > 0 || errno == EINTR)
            ;
        return 0;
    }
    int status = 0;
    for (;;) {
        jobs_reap(sh);
        Job *running = NULL;
        for (Job *j = sh->jobs; j; j = j->next)
            if (j->state == JOB_RUNNING)
                running = j;
        if (!running)
            break;
        int ev = shell_poll_input(sh);
        if (ev == EV_INTR || ev == EV_CLOSE) {
            sh_out(sh, "^C\n");
            return 130;
        }
        if (ev == EV_NONE)
            shell_wait(sh, NULL, 0, NULL, -1);
    }
    return status;
}

/* ------------------------------------------------------------- source */
static int bi_source(Shell *sh, int argc, char **argv, char **raw)
{
    (void)raw;
    if (argc < 2) {
        sh_errorf(sh, "%s: filename argument required", argv[0]);
        return 2;
    }
    if (sh->source_depth > 64) {
        sh_errorf(sh, "%s: maximum nesting level exceeded", argv[0]);
        return 1;
    }
    return run_script_file(sh, argv[1], argv + 2, argc - 2, 1);
}

/* ------------------------------------------------------- export/unset */
static int bi_export(Shell *sh, int argc, char **argv, char **raw)
{
    (void)raw;
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "-p") == 0)) {
        for (Var *v = sh->vars; v; v = v->next)
            if (v->exported)
                sh_printf(sh, "declare -x %s=\"%s\"\n", v->name, v->value);
        return 0;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        char *name = eq ? xstrndup(argv[i], (size_t)(eq - argv[i])) : xstrdup(argv[i]);
        if (!is_name_start(name[0])) {
            sh_errorf(sh, "export: `%s': not a valid identifier", argv[i]);
            status = 1;
        } else if (eq) {
            var_set(sh, name, eq + 1, 1);
        } else {
            var_export(sh, name);
        }
        free(name);
    }
    return status;
}

static int bi_unset(Shell *sh, int argc, char **argv, char **raw)
{
    (void)raw;
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-')
            var_unset(sh, argv[i]);
    return 0;
}

static int bi_shift(Shell *sh, int argc, char **argv, char **raw)
{
    (void)raw;
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (n < 0 || n > sh->posc)
        return 1;
    for (int i = 0; i < n; i++)
        free(sh->posv[i]);
    memmove(sh->posv, sh->posv + n, (size_t)(sh->posc - n + 1) * sizeof(char *));
    sh->posc -= n;
    return 0;
}

static int bi_true(Shell *sh, int argc, char **argv, char **raw)
{
    (void)sh, (void)argc, (void)argv, (void)raw;
    return 0;
}

static int bi_false(Shell *sh, int argc, char **argv, char **raw)
{
    (void)sh, (void)argc, (void)argv, (void)raw;
    return 1;
}

/* -------------------------------------------------------- test / [ ] */
static int to_int(Shell *sh, const char *s, long *out)
{
    char *end;
    while (*s == ' ')
        s++;
    *out = strtol(s, &end, 10);
    if (!*s || *end) {
        sh_errorf(sh, "test: %s: integer expression expected", s);
        return -1;
    }
    return 0;
}

static int test_unary(Shell *sh, const char *op, const char *arg)
{
    struct stat st;
    switch (op[1]) {
    case 'z': return *arg == '\0' ? 0 : 1;
    case 'n': return *arg != '\0' ? 0 : 1;
    case 'e': return stat(arg, &st) == 0 ? 0 : 1;
    case 'f': return stat(arg, &st) == 0 && S_ISREG(st.st_mode) ? 0 : 1;
    case 'd': return stat(arg, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : 1;
    case 's': return stat(arg, &st) == 0 && st.st_size > 0 ? 0 : 1;
    case 'r': return access(arg, R_OK) == 0 ? 0 : 1;
    case 'w': return access(arg, W_OK) == 0 ? 0 : 1;
    case 'x': return access(arg, X_OK) == 0 ? 0 : 1;
    case 'L':
    case 'h': return lstat(arg, &st) == 0 && S_ISLNK(st.st_mode) ? 0 : 1;
    case 'p': return stat(arg, &st) == 0 && S_ISFIFO(st.st_mode) ? 0 : 1;
    default:
        sh_errorf(sh, "test: %s: unary operator expected", op);
        return 2;
    }
}

static int is_unary_op(const char *s)
{
    return s[0] == '-' && s[1] && !s[2] && strchr("znefdsrwxLhp", s[1]);
}

static int test_binary(Shell *sh, const char *a, const char *op, const char *b)
{
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0)
        return strcmp(a, b) == 0 ? 0 : 1;
    if (strcmp(op, "!=") == 0)
        return strcmp(a, b) != 0 ? 0 : 1;
    if (strcmp(op, "<") == 0)
        return strcmp(a, b) < 0 ? 0 : 1;
    if (strcmp(op, ">") == 0)
        return strcmp(a, b) > 0 ? 0 : 1;
    if (strcmp(op, "-nt") == 0 || strcmp(op, "-ot") == 0) {
        struct stat sa, sb;
        int ha = stat(a, &sa) == 0, hb = stat(b, &sb) == 0;
        if (op[1] == 'n')
            return ha && (!hb || sa.st_mtime > sb.st_mtime) ? 0 : 1;
        return hb && (!ha || sa.st_mtime < sb.st_mtime) ? 0 : 1;
    }
    long x, y;
    static const char *ops[] = { "-eq", "-ne", "-lt", "-le", "-gt", "-ge" };
    int k = -1;
    for (int i = 0; i < 6; i++)
        if (strcmp(op, ops[i]) == 0)
            k = i;
    if (k < 0) {
        sh_errorf(sh, "test: %s: binary operator expected", op);
        return 2;
    }
    if (to_int(sh, a, &x) < 0 || to_int(sh, b, &y) < 0)
        return 2;
    int r = k == 0 ? x == y : k == 1 ? x != y : k == 2 ? x < y : k == 3 ? x <= y : k == 4 ? x > y : x >= y;
    return r ? 0 : 1;
}

static int is_binary_op(const char *s)
{
    static const char *ops[] = { "=", "==", "!=", "<", ">", "-eq", "-ne", "-lt",
                                 "-le", "-gt", "-ge", "-nt", "-ot" };
    for (size_t i = 0; i < ARRAY_LEN(ops); i++)
        if (strcmp(s, ops[i]) == 0)
            return 1;
    return 0;
}

static int test_eval(Shell *sh, char **a, int n)
{
    if (n <= 0)
        return 1;
    /* lowest precedence first: -o, then -a */
    for (int i = n - 1; i > 0; i--)
        if (strcmp(a[i], "-o") == 0 && i < n - 1) {
            int l = test_eval(sh, a, i);
            if (l == 2)
                return 2;
            return l == 0 ? 0 : test_eval(sh, a + i + 1, n - i - 1);
        }
    for (int i = n - 1; i > 0; i--)
        if (strcmp(a[i], "-a") == 0 && i < n - 1) {
            int l = test_eval(sh, a, i);
            if (l != 0)
                return l;
            return test_eval(sh, a + i + 1, n - i - 1);
        }
    if (strcmp(a[0], "!") == 0) {
        int r = test_eval(sh, a + 1, n - 1);
        return r == 2 ? 2 : !r;
    }
    if (n >= 3 && strcmp(a[0], "(") == 0 && strcmp(a[n - 1], ")") == 0)
        return test_eval(sh, a + 1, n - 2);
    switch (n) {
    case 1:
        return a[0][0] ? 0 : 1;
    case 2:
        if (is_unary_op(a[0]))
            return test_unary(sh, a[0], a[1]);
        sh_errorf(sh, "test: %s: unary operator expected", a[0]);
        return 2;
    case 3:
        if (is_binary_op(a[1]))
            return test_binary(sh, a[0], a[1], a[2]);
        sh_errorf(sh, "test: %s: binary operator expected", a[1]);
        return 2;
    default:
        sh_errorf(sh, "test: too many arguments");
        return 2;
    }
}

static int bi_test(Shell *sh, int argc, char **argv, char **raw)
{
    (void)raw;
    if (strcmp(argv[0], "[") == 0) {
        if (strcmp(argv[argc - 1], "]") != 0) {
            sh_errorf(sh, "[: missing `]'");
            return 2;
        }
        argc--;
    }
    return test_eval(sh, argv + 1, argc - 1);
}

/* -------------------------------------------------------------- clear */
static int bi_clear(Shell *sh, int argc, char **argv, char **raw)
{
    (void)argc, (void)argv, (void)raw;
    if (sh->fd_out == FD_GUI && sh->interactive) {
        sh_forward_output(sh);
        msg_send(sh->gui_out, M_CLEAR, NULL, 0, NULL, 0);
    } else {
        sh_out(sh, "\033[H\033[2J");
    }
    return 0;
}

/* --------------------------------------------------------------- bind */
static void bind_list(Shell *sh)
{
    KeyBindings kb;
    StrVec errs;
    sv_init(&errs);
    bindings_load(&kb, &errs);
    for (int i = 0; i < KA_COUNT; i++) {
        char ks[64];
        key_format(&kb.combo[i], ks, sizeof ks);
        sh_printf(sh, "%-16s %-16s %s\n", keyaction_name(i), ks, kb.custom[i] ? "(custom)" : "");
    }
    for (int i = 0; i < errs.n; i++)
        sh_printf(sh, "warning: %s\n", errs.v[i]);
    sv_free(&errs);
}

static void bind_notify(Shell *sh)
{
    if (sh->interactive && !sh->closed)
        msg_send(sh->gui_out, M_BINDRELOAD, NULL, 0, NULL, 0);
}

static int bi_bind(Shell *sh, int argc, char **argv, char **raw)
{
    (void)raw;
    char err[256];
    if (argc == 1) {
        bind_list(sh);
        return 0;
    }
    if (strcmp(argv[1], "-l") == 0) {
        for (int i = 0; i < KA_COUNT; i++)
            sh_printf(sh, "%-16s %s\n", keyaction_name(i), keyaction_help(i));
        return 0;
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        sh_out(sh, "usage: bind                   list the current key bindings\n"
                   "       bind -l                list the actions\n"
                   "       bind <action> <key>    rebind, e.g. bind line-start Ctrl+B\n"
                   "       bind -r <action>       restore one default\n"
                   "       bind -R                restore all defaults\n"
                   "Bindings are saved in ~/.terminatorrc and survive restarts.\n");
        return 0;
    }
    if (strcmp(argv[1], "-R") == 0) {
        if (rc_reset_all(err, sizeof err) < 0) {
            sh_errorf(sh, "bind: %s", err);
            return 1;
        }
        bind_notify(sh);
        sh_out(sh, "all key bindings restored to their defaults\n");
        return 0;
    }
    if (strcmp(argv[1], "-r") == 0) {
        if (argc < 3 || keyaction_from_name(argv[2]) < 0) {
            sh_errorf(sh, "bind: -r needs a valid action (see bind -l)");
            return 1;
        }
        if (rc_write_binding(keyaction_name(keyaction_from_name(argv[2])), NULL, err, sizeof err) < 0) {
            sh_errorf(sh, "bind: %s", err);
            return 1;
        }
        bind_notify(sh);
        return 0;
    }
    if (argc != 3) {
        sh_errorf(sh, "bind: usage: bind <action> <key>   (bind -h for help)");
        return 2;
    }
    int a = keyaction_from_name(argv[1]);
    if (a < 0) {
        sh_errorf(sh, "bind: unknown action '%s' (see bind -l)", argv[1]);
        return 1;
    }
    KeyCombo kc;
    if (key_parse(argv[2], &kc, err, sizeof err) < 0) {
        sh_errorf(sh, "bind: %s; '%s' keeps its current key", err, argv[1]);
        return 1;
    }
    KeyBindings kb;
    bindings_load(&kb, NULL);
    for (int i = 0; i < KA_COUNT; i++) {
        if (i != a && key_equal(&kb.combo[i], &kc)) {
            char ks[64];
            key_format(&kc, ks, sizeof ks);
            sh_errorf(sh, "bind: %s is already bound to '%s'; '%s' keeps its current key", ks,
                      keyaction_name(i), argv[1]);
            return 1;
        }
    }
    char ks[64];
    key_format(&kc, ks, sizeof ks);
    if (rc_write_binding(keyaction_name(a), ks, err, sizeof err) < 0) {
        sh_errorf(sh, "bind: %s", err);
        return 1;
    }
    bind_notify(sh);
    sh_printf(sh, "%s is now bound to %s (saved in %s)\n", keyaction_name(a), ks, rc_path());
    return 0;
}

/* --------------------------------------------------------------- help */
static int bi_help(Shell *sh, int argc, char **argv, char **raw)
{
    (void)argc, (void)argv, (void)raw;
    sh_out(sh,
           "Terminator builtins:\n"
           "  cd [dir|-]          change directory          pwd\n"
           "  echo [-neE] args    print arguments           exit [n]\n"
           "  history [n|-c]      show/clear history        source file [args] / . file\n"
           "  jobs / fg [%n] / bg [%n] / kill [-sig] pid|%n / wait\n"
           "  export NAME=value   unset NAME                shift [n]\n"
           "  test expr / [ expr ]                          true / false / :\n"
           "  multiWatch [-n secs] [\"cmd1\", \"cmd2\", ...]   run commands in parallel\n"
           "  bind [action key]   show or change key bindings (bind -h)\n"
           "  clear               clear the screen          help\n"
           "Keys: Ctrl+Shift+T new tab, Ctrl+Shift+W close tab, Ctrl+PageUp/PageDown or\n"
           "Alt+1..9 switch tabs, Shift+PageUp/PageDown scroll, Ctrl+R search history.\n");
    return 0;
}

/* ------------------------------------------------------------- lookup */
static const struct {
    const char *name;
    BuiltinFn fn;
} table[] = {
    { "cd", bi_cd },           { "pwd", bi_pwd },         { "echo", bi_echo },
    { "exit", bi_exit },       { "history", bi_history }, { "jobs", bi_jobs },
    { "fg", bi_fg },           { "bg", bi_bg },           { "kill", bi_kill },
    { "wait", bi_wait },       { "source", bi_source },   { ".", bi_source },
    { "export", bi_export },   { "unset", bi_unset },     { "shift", bi_shift },
    { "true", bi_true },       { "false", bi_false },     { ":", bi_true },
    { "test", bi_test },       { "[", bi_test },          { "clear", bi_clear },
    { "bind", bi_bind },       { "help", bi_help },       { "multiWatch", builtin_multiwatch },
};

BuiltinFn builtin_lookup(const char *name)
{
    for (size_t i = 0; i < ARRAY_LEN(table); i++)
        if (strcmp(table[i].name, name) == 0)
            return table[i].fn;
    return NULL;
}

int builtin_must_run_in_shell(const char *name)
{
    return builtin_lookup(name) != NULL;
}
