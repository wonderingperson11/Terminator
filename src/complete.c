/*
 * complete.c - Tab completion of file names in the tab's current directory.
 *
 *   one match       -> complete it (a directory gets a trailing '/')
 *   several matches -> extend to the longest common prefix; if that still
 *                      leaves several, print a numbered list and let the
 *                      user pick one by typing its number
 *   no match        -> do nothing
 *
 * The rest of the line is never touched, whether or not the command exists.
 */
#include "shell.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int is_word_break(char c)
{
    return c == ' ' || c == '\t' || c == '|' || c == ';' || c == '&' || c == '<' || c == '>' ||
           c == '(';
}

static int needs_escape(char c)
{
    return strchr(" \t\\'\"$`&|;<>()*?[]!#{}", c) != NULL;
}

static void append_escaped(Buf *b, const char *s)
{
    for (; *s; s++) {
        if (needs_escape(*s))
            buf_putc(b, '\\');
        buf_putc(b, *s);
    }
}

/* Undo shell quoting in the partial word typed so far. */
static char *unescape_word(const char *s, size_t n)
{
    Buf b;
    buf_init(&b);
    char q = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (q) {
            if (c == q)
                q = 0;
            else
                buf_putc(&b, c);
        } else if (c == '\'' || c == '"') {
            q = c;
        } else if (c == '\\' && i + 1 < n) {
            buf_putc(&b, s[++i]);
        } else {
            buf_putc(&b, c);
        }
    }
    return buf_detach(&b);
}

static void replace_word(Shell *sh, size_t start, size_t end, const char *dir, const char *name)
{
    Editor *ed = &sh->ed;
    Buf rep;
    buf_init(&rep);
    append_escaped(&rep, dir);
    append_escaped(&rep, name);
    buf_erase(&ed->line, start, end - start);
    buf_insert(&ed->line, start, rep.s ? rep.s : "", rep.len);
    ed->cur = start + rep.len;
    buf_free(&rep);
}

void complete_word(Shell *sh)
{
    Editor *ed = &sh->ed;
    const char *line = ed->line.s ? ed->line.s : "";
    size_t ws = ed->cur;
    while (ws > 0) {
        char c = line[ws - 1];
        if (is_word_break(c) && !(ws >= 2 && line[ws - 2] == '\\'))
            break;
        ws--;
    }
    char *word = unescape_word(line + ws, ed->cur - ws);

    /* split into directory part and the prefix to match */
    char *slash = strrchr(word, '/');
    char *dirpart = slash ? xstrndup(word, (size_t)(slash - word + 1)) : xstrdup("");
    const char *base = slash ? slash + 1 : word;
    char *opendir_path = *dirpart ? expand_home(dirpart) : xstrdup(".");

    StrVec names;
    sv_init(&names);
    DIR *d = opendir(opendir_path);
    if (d) {
        struct dirent *de;
        size_t bl = strlen(base);
        while ((de = readdir(d))) {
            const char *nm = de->d_name;
            if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
                continue;
            if (nm[0] == '.' && base[0] != '.')
                continue;
            if (strncmp(nm, base, bl) == 0)
                sv_pushdup(&names, nm);
        }
        closedir(d);
    }
    sv_sort(&names);

    if (names.n == 0)
        goto done;

    /* directories get a trailing '/' */
    StrVec shown;
    sv_init(&shown);
    for (int i = 0; i < names.n; i++) {
        char *full = xasprintf("%s/%s", opendir_path, names.v[i]);
        struct stat st;
        int isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        sv_push(&shown, isdir ? xasprintf("%s/", names.v[i]) : xstrdup(names.v[i]));
        free(full);
    }

    if (names.n == 1) {
        replace_word(sh, ws, ed->cur, dirpart, shown.v[0]);
        sv_free(&shown);
        goto done;
    }

    /* longest common prefix */
    size_t lcp = strlen(names.v[0]);
    for (int i = 1; i < names.n; i++) {
        size_t k = 0;
        while (k < lcp && names.v[i][k] == names.v[0][k])
            k++;
        lcp = k;
    }
    if (lcp > strlen(base)) {
        char *pre = xstrndup(names.v[0], lcp);
        replace_word(sh, ws, ed->cur, dirpart, pre);
        free(pre);
    }

    /* still ambiguous: numbered list, then wait for the user's pick */
    ed_commit_line(sh, ed->prompt, ed->line.s ? ed->line.s : "", "");
    Buf list;
    buf_init(&list);
    size_t col = 0;
    int width = sh->cols > 20 ? sh->cols : 80;
    for (int i = 0; i < shown.n; i++) {
        char item[1024];
        int len = snprintf(item, sizeof item, "%d. %s", i + 1, shown.v[i]);
        if (col > 0 && col + 3 + (size_t)len > (size_t)width) {
            buf_putc(&list, '\n');
            col = 0;
        } else if (col > 0) {
            buf_puts(&list, "   ");
            col += 3;
        }
        buf_puts(&list, item);
        col += (size_t)len;
    }
    /* no trailing newline: the cursor waits at the end of the list */
    sh_write(sh, FD_GUI, list.s, list.len);
    buf_free(&list);

    sv_free(&ed->choices);
    ed->choices = shown;
    free(ed->dir_prefix);
    ed->dir_prefix = xstrdup(dirpart);
    free(ed->sel_saved_line);
    ed->sel_saved_line = xstrdup(ed->line.s ? ed->line.s : "");
    ed->sel_saved_cur = ed->cur;
    ed->word_start = ws;
    ed->word_end = ed->cur;
    buf_clear(&ed->digits);
    ed->sub = SUB_SELECT;

done:
    sv_free(&names);
    free(word);
    free(dirpart);
    free(opendir_path);
}

static void select_finish(Shell *sh, int pick)
{
    Editor *ed = &sh->ed;
    sh_write(sh, FD_GUI, "\n", 1); /* end the list line */
    buf_clear(&ed->line);
    buf_puts(&ed->line, ed->sel_saved_line ? ed->sel_saved_line : "");
    ed->cur = ed->sel_saved_cur;
    if (pick >= 1 && pick <= ed->choices.n)
        replace_word(sh, ed->word_start, ed->word_end, ed->dir_prefix ? ed->dir_prefix : "",
                     ed->choices.v[pick - 1]);
    ed->sub = SUB_NONE;
    sv_free(&ed->choices);
    buf_clear(&ed->digits);
}

void complete_select_key(Shell *sh, int action, const char *text, size_t tlen)
{
    Editor *ed = &sh->ed;
    int n = ed->choices.n;
    switch (action) {
    case ACT_INSERT:
        for (size_t i = 0; i < tlen; i++) {
            if (text[i] < '0' || text[i] > '9') {
                select_finish(sh, 0); /* anything else cancels */
                ed_redraw(sh);
                return;
            }
            buf_putc(&ed->digits, text[i]);
        }
        {
            int k = atoi(ed->digits.s);
            /* decide as soon as the number can't grow into another choice */
            if (k >= 1 && k <= n && k * 10 > n) {
                select_finish(sh, k);
            } else if (k > n || (ed->digits.len > 0 && k == 0 && ed->digits.len >= 2)) {
                select_finish(sh, 0);
            }
        }
        break;
    case ACT_ENTER:
        select_finish(sh, ed->digits.len ? atoi(ed->digits.s) : 0);
        break;
    case ACT_BACKSPACE:
        if (ed->digits.len)
            buf_erase(&ed->digits, ed->digits.len - 1, 1);
        break;
    case ACT_COMPLETE:
        break;
    default:
        select_finish(sh, 0);
        break;
    }
    ed_redraw(sh);
}
