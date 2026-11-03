/*
 * editor.c - the line editor behind the prompt.
 *
 * The GUI turns KeyPress events into actions (according to the configurable
 * bindings) and sends them here; the editor updates its buffer and cursor
 * and sends the new edit line back (M_EDIT) for the GUI to redraw.  Keeping
 * the editor in the shell means history, the tab's working directory (for
 * completion) and the job table are all at hand in both execution modes.
 */
#include "shell.h"

#include <stdlib.h>
#include <string.h>

void ed_init(Editor *ed)
{
    memset(ed, 0, sizeof *ed);
    ed->mode = ED_OFF;
    ed->hist_pos = -1;
    buf_init(&ed->line);
    buf_init(&ed->search);
    buf_init(&ed->digits);
    sv_init(&ed->choices);
}

void ed_free(Editor *ed)
{
    free(ed->prompt);
    free(ed->saved);
    free(ed->dir_prefix);
    free(ed->sel_saved_line);
    buf_free(&ed->line);
    buf_free(&ed->search);
    buf_free(&ed->digits);
    sv_free(&ed->choices);
}

static void set_line(Editor *ed, const char *s)
{
    buf_clear(&ed->line);
    buf_puts(&ed->line, s ? s : "");
    ed->cur = ed->line.len;
}

void ed_begin(Shell *sh, int mode, const char *prompt)
{
    Editor *ed = &sh->ed;
    ed->mode = mode;
    ed->sub = SUB_NONE;
    free(ed->prompt);
    ed->prompt = xstrdup(prompt ? prompt : "");
    set_line(ed, "");
    ed->hist_pos = -1;
    free(ed->saved);
    ed->saved = NULL;
    ed_redraw(sh);
}

void ed_redraw(Shell *sh)
{
    if (!sh->interactive || sh->closed)
        return;
    Editor *ed = &sh->ed;
    const char *prompt = ed->prompt ? ed->prompt : "";
    const char *text = ed->line.s ? ed->line.s : "";
    size_t cur = ed->cur;
    if (ed->sub == SUB_SEARCH) {
        prompt = "Enter search term: ";
        text = ed->search.s ? ed->search.s : "";
        cur = ed->search.len;
    } else if (ed->sub == SUB_SELECT) {
        prompt = "";
        text = ed->digits.s ? ed->digits.s : "";
        cur = ed->digits.len;
    }
    Buf b;
    buf_init(&b);
    buf_puts(&b, prompt);
    buf_puts(&b, text);
    int32_t v[2] = { (int32_t)strlen(prompt), (int32_t)cur };
    msg_send(sh->gui_out, M_EDIT, v, sizeof v, b.s, b.len);
    buf_free(&b);
}

void ed_hide(Shell *sh)
{
    if (!sh->interactive || sh->closed)
        return;
    int32_t v[2] = { -1, 0 };
    msg_send(sh->gui_out, M_EDIT, v, sizeof v, NULL, 0);
}

/* Show "prompt + text + suffix" as a finished line in the scrollback. */
static void commit(Shell *sh, const char *prompt, const char *text, const char *suffix)
{
    if (!sh->interactive || sh->closed)
        return;
    sh_forward_output(sh);
    Buf b;
    buf_init(&b);
    buf_puts(&b, prompt);
    buf_puts(&b, text);
    buf_puts(&b, suffix);
    msg_send_int(sh->gui_out, M_COMMIT, (int32_t)strlen(prompt), b.s, b.len);
    buf_free(&b);
}

static size_t prev_char(const Buf *b, size_t pos)
{
    if (pos == 0)
        return 0;
    pos--;
    while (pos > 0 && ((unsigned char)b->s[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

static size_t next_char(const Buf *b, size_t pos)
{
    if (pos >= b->len)
        return b->len;
    pos++;
    while (pos < b->len && ((unsigned char)b->s[pos] & 0xC0) == 0x80)
        pos++;
    return pos;
}

static void insert_text(Buf *b, size_t *cur, const char *text, size_t tlen)
{
    /* drop control characters other than tab */
    Buf clean;
    buf_init(&clean);
    for (size_t i = 0; i < tlen; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x20 || c == '\t')
            buf_putc(&clean, (char)c);
    }
    if (clean.len) {
        buf_insert(b, *cur, clean.s, clean.len);
        *cur += clean.len;
    }
    buf_free(&clean);
}

static void history_move(Shell *sh, int dir)
{
    Editor *ed = &sh->ed;
    if (dir < 0) { /* older */
        char *e = hist_get_recent(ed->hist_pos + 1);
        if (!e)
            return;
        if (ed->hist_pos == -1) {
            free(ed->saved);
            ed->saved = xstrdup(ed->line.s ? ed->line.s : "");
        }
        ed->hist_pos++;
        set_line(ed, e);
        free(e);
    } else { /* newer */
        if (ed->hist_pos < 0)
            return;
        ed->hist_pos--;
        if (ed->hist_pos < 0) {
            set_line(ed, ed->saved);
        } else {
            char *e = hist_get_recent(ed->hist_pos);
            set_line(ed, e ? e : "");
            free(e);
        }
    }
}

static int handle_search(Shell *sh, int action, const char *text, size_t tlen)
{
    Editor *ed = &sh->ed;
    switch (action) {
    case ACT_INSERT: {
        size_t cur = ed->search.len;
        insert_text(&ed->search, &cur, text, tlen);
        break;
    }
    case ACT_BACKSPACE:
        if (ed->search.len)
            buf_erase(&ed->search, prev_char(&ed->search, ed->search.len),
                      ed->search.len - prev_char(&ed->search, ed->search.len));
        break;
    case ACT_KILL_START:
        buf_clear(&ed->search);
        break;
    case ACT_ENTER: {
        char *term = xstrdup(ed->search.s ? ed->search.s : "");
        commit(sh, "Enter search term: ", term, "");
        ed->sub = SUB_NONE;
        if (*term)
            hist_search(sh, term);
        free(term);
        break;
    }
    case ACT_INTR:
    case ACT_ESCAPE:
        commit(sh, "Enter search term: ", ed->search.s ? ed->search.s : "", "^C");
        ed->sub = SUB_NONE;
        break;
    default:
        break;
    }
    ed_redraw(sh);
    return EV_NONE;
}

int ed_handle(Shell *sh, int action, const char *text, size_t tlen)
{
    Editor *ed = &sh->ed;
    Buf *b = &ed->line;

    if (ed->mode == ED_OFF) {
        if (action == ACT_INTR)
            return EV_INTR;
        if (action == ACT_SUSP)
            return EV_SUSP;
        if (action == ACT_CLEAR && sh->interactive)
            msg_send(sh->gui_out, M_CLEAR, NULL, 0, NULL, 0);
        return EV_NONE;
    }
    if (ed->sub == SUB_SEARCH)
        return handle_search(sh, action, text, tlen);
    if (ed->sub == SUB_SELECT) {
        complete_select_key(sh, action, text, tlen);
        return EV_NONE;
    }

    int prompt_mode = ed->mode == ED_PROMPT;
    switch (action) {
    case ACT_INSERT:
        insert_text(b, &ed->cur, text, tlen);
        break;
    case ACT_ENTER: {
        char *line = xstrdup(b->s ? b->s : "");
        commit(sh, ed->prompt, line, "");
        free(sh->ready_line);
        sh->ready_line = line;
        set_line(ed, "");
        ed->hist_pos = -1;
        if (prompt_mode) {
            /* the command runs now; show a bare cursor until it's done */
            free(ed->prompt);
            ed->prompt = xstrdup("");
        }
        ed_redraw(sh);
        return EV_LINE;
    }
    case ACT_BACKSPACE:
        if (ed->cur > 0) {
            size_t p = prev_char(b, ed->cur);
            buf_erase(b, p, ed->cur - p);
            ed->cur = p;
        }
        break;
    case ACT_DELETE:
        if (ed->cur < b->len)
            buf_erase(b, ed->cur, next_char(b, ed->cur) - ed->cur);
        break;
    case ACT_LEFT:
        ed->cur = prev_char(b, ed->cur);
        break;
    case ACT_RIGHT:
        ed->cur = next_char(b, ed->cur);
        break;
    case ACT_HOME:
        ed->cur = 0;
        break;
    case ACT_END:
        ed->cur = b->len;
        break;
    case ACT_HIST_PREV:
        if (prompt_mode)
            history_move(sh, -1);
        break;
    case ACT_HIST_NEXT:
        if (prompt_mode)
            history_move(sh, +1);
        break;
    case ACT_KILL_START:
        buf_erase(b, 0, ed->cur);
        ed->cur = 0;
        break;
    case ACT_KILL_END:
        buf_erase(b, ed->cur, b->len - ed->cur);
        break;
    case ACT_DEL_WORD: {
        size_t p = ed->cur;
        while (p > 0 && (b->s[p - 1] == ' ' || b->s[p - 1] == '\t'))
            p--;
        while (p > 0 && b->s[p - 1] != ' ' && b->s[p - 1] != '\t')
            p--;
        buf_erase(b, p, ed->cur - p);
        ed->cur = p;
        break;
    }
    case ACT_CLEAR:
        if (sh->interactive)
            msg_send(sh->gui_out, M_CLEAR, NULL, 0, NULL, 0);
        break;
    case ACT_EOF:
        if (prompt_mode && b->len > 0) {
            /* like bash: Ctrl+D on a non-empty line deletes forward */
            if (ed->cur < b->len)
                buf_erase(b, ed->cur, next_char(b, ed->cur) - ed->cur);
            break;
        }
        return EV_EOF;
    case ACT_INTR:
        if (prompt_mode) {
            commit(sh, ed->prompt, b->s ? b->s : "", "^C");
            set_line(ed, "");
            ed->hist_pos = -1;
        } else {
            set_line(ed, "");
        }
        ed_redraw(sh);
        return EV_INTR;
    case ACT_SUSP:
        if (prompt_mode)
            break; /* nothing to suspend; Terminator itself never stops */
        set_line(ed, "");
        ed_redraw(sh);
        return EV_SUSP;
    case ACT_SEARCH:
        if (prompt_mode) {
            commit(sh, ed->prompt, b->s ? b->s : "", "");
            ed->sub = SUB_SEARCH;
            buf_clear(&ed->search);
        }
        break;
    case ACT_COMPLETE:
        if (prompt_mode)
            complete_word(sh);
        break;
    default:
        break;
    }
    ed_redraw(sh);
    return EV_NONE;
}

void ed_commit_line(Shell *sh, const char *prompt, const char *text, const char *suffix)
{
    commit(sh, prompt, text, suffix);
}
