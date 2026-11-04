/*
 * keys.c - key binding table, parser and ~/.terminatorrc handling.
 * Only X keysym *constants* are used here (no Xlib calls), so the shell
 * side (the `bind` builtin) can validate bindings too.
 */
#include "keys.h"
#include "proto.h"

#include <X11/keysym.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <unistd.h>

typedef struct {
    const char *name;
    const char *def;
    int shell_action;
    const char *help;
} ActionInfo;

static const ActionInfo actions[KA_COUNT] = {
    [KA_CURSOR_LEFT] = { "cursor-left", "Left", ACT_LEFT, "move left within the line" },
    [KA_CURSOR_RIGHT] = { "cursor-right", "Right", ACT_RIGHT, "move right within the line" },
    [KA_LINE_START] = { "line-start", "Ctrl+A", ACT_HOME, "move to the start of the line" },
    [KA_LINE_END] = { "line-end", "Ctrl+E", ACT_END, "move to the end of the line" },
    [KA_HISTORY_PREV] = { "history-prev", "Up", ACT_HIST_PREV, "previous command in history" },
    [KA_HISTORY_NEXT] = { "history-next", "Down", ACT_HIST_NEXT, "next command in history" },
    [KA_KILL_START] = { "kill-line-start", "Ctrl+U", ACT_KILL_START, "delete to start of line" },
    [KA_KILL_END] = { "kill-line-end", "Ctrl+K", ACT_KILL_END, "delete to end of line" },
    [KA_DELETE_WORD] = { "delete-word", "Ctrl+W", ACT_DEL_WORD, "delete the previous word" },
    [KA_CLEAR_SCREEN] = { "clear-screen", "Ctrl+L", ACT_CLEAR, "clear the screen" },
    [KA_EOF] = { "eof", "Ctrl+D", ACT_EOF, "exit the shell / send EOF" },
    [KA_COPY] = { "copy", "Ctrl+Shift+C", 0, "copy selection to the clipboard" },
    [KA_PASTE] = { "paste", "Ctrl+Shift+V", 0, "paste from the clipboard" },
    [KA_INTERRUPT] = { "interrupt", "Ctrl+C", ACT_INTR, "interrupt the running command" },
    [KA_SUSPEND] = { "suspend", "Ctrl+Z", ACT_SUSP, "suspend the running command" },
    [KA_HISTORY_SEARCH] = { "history-search", "Ctrl+R", ACT_SEARCH, "search the history" },
    [KA_COMPLETE] = { "complete", "Tab", ACT_COMPLETE, "complete a file name" },
    [KA_NEW_TAB] = { "new-tab", "Ctrl+Shift+T", 0, "open a new tab" },
    [KA_CLOSE_TAB] = { "close-tab", "Ctrl+Shift+W", 0, "close the current tab" },
    [KA_NEXT_TAB] = { "next-tab", "Ctrl+PageDown", 0, "switch to the next tab" },
    [KA_PREV_TAB] = { "prev-tab", "Ctrl+PageUp", 0, "switch to the previous tab" },
    [KA_SCROLL_UP] = { "scroll-up", "Shift+PageUp", 0, "scroll the output up" },
    [KA_SCROLL_DOWN] = { "scroll-down", "Shift+PageDown", 0, "scroll the output down" },
};

static const struct {
    const char *name;
    unsigned long sym;
} named[] = {
    { "Left", XK_Left },       { "Right", XK_Right },       { "Up", XK_Up },
    { "Down", XK_Down },       { "Home", XK_Home },         { "End", XK_End },
    { "Tab", XK_Tab },         { "PageUp", XK_Prior },      { "Prior", XK_Prior },
    { "PageDown", XK_Next },   { "Next", XK_Next },         { "Insert", XK_Insert },
    { "Delete", XK_Delete },   { "Escape", XK_Escape },     { "Esc", XK_Escape },
    { "Space", XK_space },     { "Backspace", XK_BackSpace }, { "Enter", XK_Return },
    { "Return", XK_Return },   { "F1", XK_F1 },             { "F2", XK_F2 },
    { "F3", XK_F3 },           { "F4", XK_F4 },             { "F5", XK_F5 },
    { "F6", XK_F6 },           { "F7", XK_F7 },             { "F8", XK_F8 },
    { "F9", XK_F9 },           { "F10", XK_F10 },           { "F11", XK_F11 },
    { "F12", XK_F12 },
};

const char *keyaction_name(int a)
{
    return a >= 0 && a < KA_COUNT ? actions[a].name : "?";
}

const char *keyaction_help(int a)
{
    return a >= 0 && a < KA_COUNT ? actions[a].help : "";
}

int keyaction_from_name(const char *name)
{
    for (int i = 0; i < KA_COUNT; i++)
        if (strcasecmp(actions[i].name, name) == 0)
            return i;
    return -1;
}

int keyaction_shell_action(int a)
{
    return a >= 0 && a < KA_COUNT ? actions[a].shell_action : 0;
}

unsigned long key_normalise_sym(unsigned long sym)
{
    if (sym >= XK_A && sym <= XK_Z)
        return sym - XK_A + XK_a;
    if (sym == XK_ISO_Left_Tab)
        return XK_Tab;
    if (sym == XK_KP_Prior)
        return XK_Prior;
    if (sym == XK_KP_Left)
        return XK_Left;
    if (sym == XK_KP_Right)
        return XK_Right;
    if (sym == XK_KP_Up)
        return XK_Up;
    if (sym == XK_KP_Down)
        return XK_Down;
    if (sym == XK_KP_Next)
        return XK_Next;
    return sym;
}

int key_equal(const KeyCombo *a, const KeyCombo *b)
{
    return a->mods == b->mods && a->sym == b->sym;
}

int key_parse(const char *spec, KeyCombo *out, char *err, size_t errlen)
{
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s", spec);
    out->mods = 0;
    out->sym = 0;
    char *p = tmp;
    for (;;) {
        char *plus = strchr(p, '+');
        if (!plus || plus[1] == '\0') /* "Ctrl++" means the '+' key */
            break;
        *plus = '\0';
        if (strcasecmp(p, "ctrl") == 0 || strcasecmp(p, "control") == 0 || strcasecmp(p, "c") == 0)
            out->mods |= KMOD_CTRL;
        else if (strcasecmp(p, "shift") == 0 || strcasecmp(p, "s") == 0)
            out->mods |= KMOD_SHIFT;
        else if (strcasecmp(p, "alt") == 0 || strcasecmp(p, "meta") == 0 || strcasecmp(p, "m") == 0)
            out->mods |= KMOD_ALT;
        else {
            snprintf(err, errlen, "unknown modifier '%s' in '%s'", p, spec);
            return -1;
        }
        p = plus + 1;
    }
    if (*p == '\0') {
        snprintf(err, errlen, "missing key name in '%s'", spec);
        return -1;
    }
    if (strlen(p) == 1 && isprint((unsigned char)*p)) {
        out->sym = (unsigned long)(unsigned char)tolower((unsigned char)*p);
    } else {
        for (size_t i = 0; i < ARRAY_LEN(named); i++)
            if (strcasecmp(named[i].name, p) == 0)
                out->sym = named[i].sym;
        if (!out->sym) {
            snprintf(err, errlen, "unknown key '%s'", p);
            return -1;
        }
    }
    /* keys needed for plain typing cannot be taken over */
    if (out->sym == XK_Return || out->sym == XK_BackSpace) {
        snprintf(err, errlen, "'%s' is reserved", spec);
        return -1;
    }
    if (!(out->mods & (KMOD_CTRL | KMOD_ALT)) && out->sym >= 0x20 && out->sym <= 0x7e) {
        snprintf(err, errlen, "'%s' would shadow normal typing (add Ctrl or Alt)", spec);
        return -1;
    }
    return 0;
}

void key_format(const KeyCombo *k, char *out, size_t outlen)
{
    char name[32] = "";
    for (size_t i = 0; i < ARRAY_LEN(named); i++) {
        if (named[i].sym == k->sym) {
            snprintf(name, sizeof name, "%s", named[i].name);
            break;
        }
    }
    if (!name[0]) {
        if (k->sym >= 0x20 && k->sym <= 0x7e)
            snprintf(name, sizeof name, "%c", toupper((int)k->sym));
        else
            snprintf(name, sizeof name, "0x%lx", k->sym);
    }
    snprintf(out, outlen, "%s%s%s%s", k->mods & KMOD_CTRL ? "Ctrl+" : "",
             k->mods & KMOD_ALT ? "Alt+" : "", k->mods & KMOD_SHIFT ? "Shift+" : "", name);
}

void bindings_defaults(KeyBindings *kb)
{
    char err[128];
    for (int i = 0; i < KA_COUNT; i++) {
        key_parse(actions[i].def, &kb->combo[i], err, sizeof err);
        kb->custom[i] = 0;
    }
}

const char *rc_path(void)
{
    static char path[4096];
    const char *env = getenv("TERMINATORRC");
    if (env && *env)
        snprintf(path, sizeof path, "%s", env);
    else
        snprintf(path, sizeof path, "%s/.terminatorrc", home_dir());
    return path;
}

void bindings_load(KeyBindings *kb, StrVec *errors)
{
    bindings_defaults(kb);
    size_t len;
    char *data = read_file(rc_path(), &len);
    if (!data)
        return;
    int order[KA_COUNT] = { 0 };
    int lineno = 0;
    char *next = data;
    while (next) {
        char *line = next;
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        next = nl ? nl + 1 : NULL;
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\0' || *p == '\r')
            continue;
        char word[64], act[64], key[128];
        int n = sscanf(p, "%63s %63s %127s", word, act, key);
        char err[256];
        if (n < 1 || strcmp(word, "bind") != 0) {
            if (errors)
                sv_push(errors, xasprintf("%s: line %d: unknown setting '%s'", rc_path(), lineno, word));
            continue;
        }
        if (n < 3) {
            if (errors)
                sv_push(errors, xasprintf("%s: line %d: usage: bind <action> <key>", rc_path(), lineno));
            continue;
        }
        int a = keyaction_from_name(act);
        if (a < 0) {
            if (errors)
                sv_push(errors, xasprintf("%s: line %d: unknown action '%s'", rc_path(), lineno, act));
            continue;
        }
        KeyCombo kc;
        if (key_parse(key, &kc, err, sizeof err) < 0) {
            if (errors)
                sv_push(errors, xasprintf("%s: line %d: %s; '%s' stays on its default %s", rc_path(),
                                          lineno, err, act, actions[a].def));
            continue;
        }
        kb->combo[a] = kc;
        kb->custom[a] = 1;
        order[a] = lineno;
    }
    free(data);

    /* Conflicts: the later custom binding loses and falls back to its
     * default; repeat, since a restored default may itself collide. */
    for (int guard = 0; guard < KA_COUNT * 2; guard++) {
        int loser = -1, other = -1;
        for (int i = 0; i < KA_COUNT && loser < 0; i++) {
            for (int j = i + 1; j < KA_COUNT; j++) {
                if (!key_equal(&kb->combo[i], &kb->combo[j]))
                    continue;
                if (kb->custom[i] && (!kb->custom[j] || order[i] > order[j])) {
                    loser = i;
                    other = j;
                } else {
                    loser = j;
                    other = i;
                }
                break;
            }
        }
        if (loser < 0)
            break;
        if (!kb->custom[loser])
            break; /* two defaults never collide */
        char ks[64];
        key_format(&kb->combo[loser], ks, sizeof ks);
        if (errors)
            sv_push(errors, xasprintf("%s: %s for '%s' conflicts with '%s'; '%s' stays on its default %s",
                                      rc_path(), ks, actions[loser].name, actions[other].name,
                                      actions[loser].name, actions[loser].def));
        char err[64];
        key_parse(actions[loser].def, &kb->combo[loser], err, sizeof err);
        kb->custom[loser] = 0;
        order[loser] = 0;
    }
}

int bindings_lookup(const KeyBindings *kb, const KeyCombo *k)
{
    for (int i = 0; i < KA_COUNT; i++)
        if (key_equal(&kb->combo[i], k))
            return i;
    return -1;
}

static int rc_update(const char *action, const char *key, int reset_all, char *err, size_t errlen)
{
    const char *path = rc_path();
    int lockfd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lockfd < 0) {
        snprintf(err, errlen, "%s: cannot open", path);
        return -1;
    }
    flock(lockfd, LOCK_EX);
    size_t len;
    char *data = read_file(path, &len);
    Buf out;
    buf_init(&out);
    if (!data || !*data)
        buf_puts(&out, "# Terminator key bindings: bind <action> <key>\n"
                       "# Run `bind -l` inside Terminator for the list of actions.\n");
    char *p = data;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        char word[64] = "", act[64] = "";
        char line[512];
        snprintf(line, sizeof line, "%.*s", (int)l, p);
        sscanf(line, "%63s %63s", word, act);
        int drop = strcmp(word, "bind") == 0 && (reset_all || strcasecmp(act, action) == 0);
        if (!drop) {
            buf_putn(&out, p, l);
            buf_putc(&out, '\n');
        }
        p = nl ? nl + 1 : NULL;
    }
    if (key)
        buf_printf(&out, "bind %s %s\n", action, key);
    free(data);

    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp%ld", path, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int ok = fd >= 0 && write_all(fd, out.s ? out.s : "", out.len) == 0;
    if (fd >= 0)
        close(fd);
    if (ok)
        ok = rename(tmp, path) == 0;
    else
        unlink(tmp);
    buf_free(&out);
    flock(lockfd, LOCK_UN);
    close(lockfd);
    if (!ok) {
        snprintf(err, errlen, "%s: cannot write", path);
        return -1;
    }
    return 0;
}

int rc_write_binding(const char *action, const char *key, char *err, size_t errlen)
{
    return rc_update(action, key, 0, err, errlen);
}

int rc_reset_all(char *err, size_t errlen)
{
    return rc_update("", NULL, 1, err, errlen);
}
