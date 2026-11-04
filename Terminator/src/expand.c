/*
 * expand.c - word expansion: quotes, $VAR / ${VAR}, special parameters,
 * $(...) and `...` command substitution, ~, field splitting and globbing.
 *
 * Quoting rules follow bash: single quotes are literal; double quotes keep
 * $-expansions and substitutions active but suppress splitting and
 * globbing.  One documented deviation, required by the project handout: a
 * backslash-newline inside double quotes keeps the newline.
 */
#include "shell.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    Shell *sh;
    int flags;
    Buf val;      /* the field as it will be passed on */
    Buf pat;      /* the same, as a glob pattern with quoted chars escaped */
    int has_glob; /* an unquoted * ? [ appeared */
    int started;  /* quotes seen: emit even if empty */
    StrVec *out;
} Exp;

static int is_glob_char(char c)
{
    return c == '*' || c == '?' || c == '[';
}

static void add_ch(Exp *e, char c, int quoted)
{
    buf_putc(&e->val, c);
    if (quoted && (is_glob_char(c) || c == '\\' || c == ']')) {
        buf_putc(&e->pat, '\\');
        buf_putc(&e->pat, c);
    } else {
        buf_putc(&e->pat, c);
        if (!quoted && is_glob_char(c))
            e->has_glob = 1;
    }
}

static void emit(Exp *e)
{
    if (e->val.len == 0 && !e->started)
        return;
    if (e->has_glob && !(e->flags & EXP_NOGLOB)) {
        glob_t g;
        memset(&g, 0, sizeof g);
        if (glob(e->pat.s, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
            for (size_t i = 0; i < g.gl_pathc; i++)
                sv_pushdup(e->out, g.gl_pathv[i]);
            globfree(&g);
            goto reset;
        }
        globfree(&g);
    }
    sv_pushdup(e->out, e->val.s ? e->val.s : "");
reset:
    buf_clear(&e->val);
    buf_clear(&e->pat);
    e->has_glob = 0;
    e->started = 0;
}

static int is_ifs(char c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

/* Result of an expansion: quoted => one piece of the current field;
 * unquoted => subject to field splitting (unless EXP_NOSPLIT). */
static void add_expansion(Exp *e, const char *s, int quoted)
{
    if (quoted) {
        e->started = 1;
        for (; *s; s++)
            add_ch(e, *s, 1);
        return;
    }
    for (; *s; s++) {
        if (is_ifs(*s) && !(e->flags & EXP_NOSPLIT)) {
            emit(e);
            continue;
        }
        add_ch(e, *s, 0);
    }
}

static char *param_value(Shell *sh, const char *name)
{
    char tmp[32];
    if (strcmp(name, "?") == 0) {
        snprintf(tmp, sizeof tmp, "%d", sh->last_status);
        return xstrdup(tmp);
    }
    if (strcmp(name, "#") == 0) {
        snprintf(tmp, sizeof tmp, "%d", sh->posc);
        return xstrdup(tmp);
    }
    if (strcmp(name, "$") == 0) {
        snprintf(tmp, sizeof tmp, "%ld", (long)sh->shell_pid);
        return xstrdup(tmp);
    }
    if (strcmp(name, "!") == 0) {
        if (!sh->last_bg)
            return xstrdup("");
        snprintf(tmp, sizeof tmp, "%ld", (long)sh->last_bg);
        return xstrdup(tmp);
    }
    if (strcmp(name, "0") == 0)
        return xstrdup(sh->arg0 ? sh->arg0 : "terminator");
    if (name[0] >= '1' && name[0] <= '9') {
        int k = atoi(name);
        return xstrdup(k <= sh->posc ? sh->posv[k - 1] : "");
    }
    if (strcmp(name, "*") == 0 || strcmp(name, "@") == 0) {
        Buf b;
        buf_init(&b);
        for (int i = 0; i < sh->posc; i++) {
            if (i)
                buf_putc(&b, ' ');
            buf_puts(&b, sh->posv[i]);
        }
        return buf_detach(&b);
    }
    const char *v = var_get(sh, name);
    return xstrdup(v ? v : "");
}

static int param_is_set(Shell *sh, const char *name)
{
    if (name[0] >= '1' && name[0] <= '9')
        return atoi(name) <= sh->posc;
    if (strchr("?#$!0*@", name[0]) && name[1] == '\0')
        return 1;
    return var_get(sh, name) != NULL;
}

/* ${...} forms: ${N}, ${#N}, ${N:-w}, ${N-w}, ${N:=w}, ${N:+w} */
static char *expand_brace(Shell *sh, const char *body)
{
    if (body[0] == '#' && body[1]) {
        char *v = param_value(sh, body + 1);
        char *r = xasprintf("%zu", strlen(v));
        free(v);
        return r;
    }
    size_t nl = 0;
    if (is_name_start(body[0])) {
        while (is_name_char(body[nl]))
            nl++;
    } else if (body[0] >= '0' && body[0] <= '9') {
        while (body[nl] >= '0' && body[nl] <= '9')
            nl++;
    } else if (body[0]) {
        nl = 1;
    }
    char *name = xstrndup(body, nl);
    const char *op = body + nl;
    char *val = param_value(sh, name);
    int set = param_is_set(sh, name);
    char *res = val;
    if (*op) {
        int colon = op[0] == ':';
        const char *o = op + colon;
        int use_alt = colon ? (!set || !*val) : !set;
        if (*o == '-' || *o == '=') {
            if (use_alt) {
                res = expand_str(sh, o + 1);
                if (*o == '=' && is_name_start(name[0]))
                    var_set(sh, name, res, 0);
                free(val);
            }
        } else if (*o == '+') {
            res = use_alt ? xstrdup("") : expand_str(sh, o + 1);
            free(val);
        }
    }
    free(name);
    return res;
}

/* Parse a $-expression starting at raw[i] == '$'.  Returns the index after
 * it and appends the expansion. */
static size_t expand_dollar(Exp *e, const char *raw, size_t i, size_t n, int quoted)
{
    Shell *sh = e->sh;
    char c = i + 1 < n ? raw[i + 1] : '\0';
    if (c == '(') {
        long end = scan_subst(raw, i, n);
        if (end < 0)
            end = (long)n;
        char *inner = xstrndup(raw + i + 2, (size_t)end - i - 3);
        char *res = command_subst(sh, inner);
        add_expansion(e, res, quoted);
        free(inner);
        free(res);
        return (size_t)end;
    }
    if (c == '{') {
        long end = scan_brace(raw, i, n);
        if (end < 0)
            end = (long)n;
        char *body = xstrndup(raw + i + 2, (size_t)end - i - 3);
        char *res = expand_brace(sh, body);
        add_expansion(e, res, quoted);
        free(body);
        free(res);
        return (size_t)end;
    }
    if (c == '@' && quoted) {
        /* "$@": one field per positional parameter */
        for (int k = 0; k < sh->posc; k++) {
            if (k)
                emit(e);
            add_expansion(e, sh->posv[k], 1);
        }
        if (sh->posc == 0 && e->val.len == 0)
            e->started = 0;
        return i + 2;
    }
    if (c && strchr("?#$!*@0123456789", c)) {
        char name[2] = { c, '\0' };
        char *v = param_value(sh, name);
        add_expansion(e, v, quoted);
        free(v);
        return i + 2;
    }
    if (is_name_start(c)) {
        size_t j = i + 1;
        while (j < n && is_name_char(raw[j]))
            j++;
        char *name = xstrndup(raw + i + 1, j - i - 1);
        char *v = param_value(sh, name);
        add_expansion(e, v, quoted);
        free(v);
        free(name);
        return j;
    }
    add_ch(e, '$', quoted);
    return i + 1;
}

static size_t expand_backtick(Exp *e, const char *raw, size_t i, size_t n, int quoted)
{
    long end = scan_backtick(raw, i, n);
    if (end < 0)
        end = (long)n;
    Buf inner;
    buf_init(&inner);
    for (size_t k = i + 1; k + 1 < (size_t)end; k++) {
        if (raw[k] == '\\' && k + 2 < (size_t)end + 1 &&
            (raw[k + 1] == '`' || raw[k + 1] == '\\' || raw[k + 1] == '$')) {
            buf_putc(&inner, raw[k + 1]);
            k++;
        } else {
            buf_putc(&inner, raw[k]);
        }
    }
    char *res = command_subst(e->sh, inner.s ? inner.s : "");
    add_expansion(e, res, quoted);
    free(res);
    buf_free(&inner);
    return (size_t)end;
}

static size_t expand_tilde(Exp *e, const char *raw, size_t n)
{
    size_t j = 1;
    while (j < n && raw[j] != '/' && (is_name_char(raw[j]) || raw[j] == '-' || raw[j] == '.'))
        j++;
    if (j < n && raw[j] != '/')
        return 0; /* "~foo$x": not a tilde prefix */
    char *home = NULL;
    if (j == 1) {
        const char *h = var_get(e->sh, "HOME");
        home = xstrdup(h ? h : home_dir());
    } else {
        char *user = xstrndup(raw + 1, j - 1);
        struct passwd pw, *res = NULL;
        char tmp[4096];
        if (getpwnam_r(user, &pw, tmp, sizeof tmp, &res) == 0 && res)
            home = xstrdup(pw.pw_dir);
        free(user);
    }
    if (!home)
        return 0;
    for (char *h = home; *h; h++)
        add_ch(e, *h, 1);
    e->started = 1;
    free(home);
    return j;
}

int expand_word(Shell *sh, const char *raw, StrVec *out, int flags)
{
    Exp e;
    e.sh = sh;
    e.flags = flags;
    buf_init(&e.val);
    buf_init(&e.pat);
    e.has_glob = 0;
    e.started = 0;
    e.out = out;

    size_t n = strlen(raw), i = 0;
    int in_dq = 0;
    if (n > 0 && raw[0] == '~')
        i = expand_tilde(&e, raw, n);
    while (i < n) {
        char c = raw[i];
        if (!in_dq) {
            if (c == '\\') {
                if (i + 1 < n && raw[i + 1] == '\n') {
                    i += 2;
                } else if (i + 1 < n) {
                    add_ch(&e, raw[i + 1], 1);
                    i += 2;
                } else {
                    add_ch(&e, '\\', 1);
                    i++;
                }
            } else if (c == '\'') {
                e.started = 1;
                size_t j = i + 1;
                while (j < n && raw[j] != '\'')
                    add_ch(&e, raw[j++], 1);
                i = j + 1;
            } else if (c == '"') {
                in_dq = 1;
                e.started = 1;
                i++;
            } else if (c == '$') {
                i = expand_dollar(&e, raw, i, n, 0);
            } else if (c == '`') {
                i = expand_backtick(&e, raw, i, n, 0);
            } else {
                add_ch(&e, c, 0);
                i++;
            }
        } else {
            if (c == '"') {
                in_dq = 0;
                i++;
            } else if (c == '\\' && i + 1 < n) {
                char nx = raw[i + 1];
                if (nx == '$' || nx == '`' || nx == '"' || nx == '\\' || nx == '\n') {
                    add_ch(&e, nx, 1);
                } else {
                    add_ch(&e, '\\', 1);
                    add_ch(&e, nx, 1);
                }
                i += 2;
            } else if (c == '$') {
                i = expand_dollar(&e, raw, i, n, 1);
            } else if (c == '`') {
                i = expand_backtick(&e, raw, i, n, 1);
            } else {
                add_ch(&e, c, 1);
                i++;
            }
        }
    }
    emit(&e);
    buf_free(&e.val);
    buf_free(&e.pat);
    return 0;
}

char *expand_str(Shell *sh, const char *raw)
{
    StrVec sv;
    sv_init(&sv);
    expand_word(sh, raw, &sv, EXP_NOSPLIT | EXP_NOGLOB);
    Buf b;
    buf_init(&b);
    for (int i = 0; i < sv.n; i++) {
        if (i)
            buf_putc(&b, ' ');
        buf_puts(&b, sv.v[i]);
    }
    sv_free(&sv);
    return buf_detach(&b);
}

/* ----------------------------------------------------- command substitution */

char *command_subst(Shell *sh, const char *src)
{
    int p[2];
    if (make_pipe(p, 1) < 0) {
        sh_errorf(sh, "pipe: %s", strerror(errno));
        return xstrdup("");
    }
    int interactive = sh->interactive;
    pid_t pid = shell_fork(sh);
    if (pid < 0) {
        sh_errorf(sh, "fork: %s", strerror(errno));
        close(p[0]);
        close(p[1]);
        return xstrdup("");
    }
    if (pid == 0) {
        if (interactive) {
            setpgid(0, 0); /* own group so Ctrl+C can reach it */
            int dn = open("/dev/null", O_RDONLY);
            if (dn >= 0) {
                dup2(dn, 0);
                close(dn);
            }
            dup2(sh->out_w, 2);
        }
        dup2(p[1], 1);
        shell_enter_subshell(sh);
        subshell_run_string(sh, src);
    }
    if (interactive)
        setpgid(pid, pid);
    close(p[1]);

    Buf b;
    buf_init(&b);
    char tmp[4096];
    if (interactive) {
        set_nonblock(p[0], 1);
        int killed = 0;
        for (;;) {
            int ev = shell_poll_input(sh);
            if (ev == EV_INTR || (ev == EV_CLOSE && !killed)) {
                kill(-pid, ev == EV_CLOSE ? SIGKILL : SIGINT);
                sh->interrupted = 1;
                killed |= ev == EV_CLOSE;
                if (ev == EV_INTR)
                    sh_out(sh, "^C\n");
            }
            if (ev != EV_NONE && ev != EV_CLOSE)
                continue;
            ssize_t r = read(p[0], tmp, sizeof tmp);
            if (r > 0) {
                buf_putn(&b, tmp, (size_t)r);
                continue;
            }
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR))
                break;
            int fds[1] = { p[0] };
            shell_wait(sh, fds, 1, NULL, -1);
        }
    } else {
        ssize_t r;
        while ((r = read_retry(p[0], tmp, sizeof tmp)) > 0)
            buf_putn(&b, tmp, (size_t)r);
    }
    close(p[0]);
    int st = wait_pid_blocking(pid);
    sh->subst_ran = 1;
    sh->subst_status = status_to_code(st);
    while (b.len > 0 && b.s[b.len - 1] == '\n')
        b.s[--b.len] = '\0';
    return buf_detach(&b);
}
