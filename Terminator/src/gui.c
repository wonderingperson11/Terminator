#include "gui.h"
#include "keys.h"
#include "tab.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_TABS 32
#define MAX_LINES 10000
#define PAD 6

typedef struct {
    char *s;
    int len;
    int pstart, plen;
} Line;

typedef struct {
    Line *ring;
    int cap, head, count;
    long base;     /* absolute number of the oldest kept line */
    Buf partial;   /* the current, unterminated line */
    int col;       /* write position in partial (\r and \b move it) */
    int pstart, plen;
    int esc;       /* escape-sequence parser state */
    char csi[32];
    int csilen;
    unsigned char u8[4];
    int u8len;
} TermBuf;

typedef struct {
    int id;
    TabConn conn;
    TermBuf tb;
    char *title;
    int edit_visible;
    Buf edit_prompt;  /* display bytes */
    Buf edit_text;    /* display bytes */
    int edit_cursor;  /* column inside edit_text */
    int scroll;       /* rows scrolled back from the bottom */
    int closing;      /* close requested, waiting for the shell to exit */
    int dead;         /* the shell is gone (crashed) but the tab stays */
    int bar_x0, bar_x1;
    int clear_x0, clear_x1;

} Tab;

/* A logical line as drawn (scrollback line or a line of the edit area). */
typedef struct {
    const char *s;
    int len;
    int pstart, plen;
    int cursor; /* column of the cursor, or -1 */
    long abs;
} LLine;

/* One screen row of a (wrapped) logical line. */
typedef struct {
    const char *s; /* start of the logical line */
    int off, len;  /* this row covers s[off .. off+len) */
    int pstart, plen;
    int cursor;    /* column within the row, or -1 */
    long abs;
} VRow;

/* ------------------------------------------------------------- state */

static Display *dpy;
static int screen;
static Window win;
static Pixmap back;
static GC gc;
static XFontStruct *font;
static int cw, ch, ascent;
static int win_w, win_h, bar_h;
static int cols = 80, rows = 24;
static int focused = 1;
static int process_mode;
static int dirty = 1;
static int quitting;
static long long quit_started;

static unsigned long col_bg, col_fg, col_prompt, col_bar, col_tab, col_tab_active, col_tab_text,
    col_tab_text_active, col_cursor, col_sel, col_dead;

static Atom a_wm_delete, a_wm_protocols, a_clipboard, a_utf8, a_targets, a_text, a_paste_prop;

static Tab *tabs[MAX_TABS];
static int ntabs;
static Tab *active;
static int next_tab_id = 1;
static int plus_x0, plus_x1;

static KeyBindings kb;

/* selection, in (absolute line, column) coordinates */
static int sel_active, sel_dragging;
static long sel_a_abs, sel_b_abs;
static int sel_a_off, sel_b_off;
static char *primary_text, *clip_text;
static Tab *paste_target;
static Atom paste_target_type;

/* per-frame caches (only the active tab is drawn) */
static Buf bottom_store[64];
static LLine bottom[64];
static int nbottom;
static VRow *frame_rows;
static int frame_nrows, frame_cap;

/* -------------------------------------------------------- term buffer */

static void tb_init(TermBuf *tb)
{
    memset(tb, 0, sizeof *tb);
    tb->cap = MAX_LINES;
    tb->ring = xcalloc((size_t)tb->cap, sizeof(Line));
    buf_init(&tb->partial);
}

static void tb_free(TermBuf *tb)
{
    for (int i = 0; i < tb->count; i++)
        free(tb->ring[(tb->head + i) % tb->cap].s);
    free(tb->ring);
    buf_free(&tb->partial);
}

static Line *tb_line(TermBuf *tb, int i)
{
    return &tb->ring[(tb->head + i) % tb->cap];
}

static void tb_clear(TermBuf *tb)
{
    for (int i = 0; i < tb->count; i++)
        free(tb_line(tb, i)->s);
    tb->base += tb->count;
    tb->count = 0;
    tb->head = 0;
    buf_clear(&tb->partial);
    tb->col = 0;
    tb->pstart = tb->plen = 0;
}

static void tb_newline(TermBuf *tb)
{
    Line l;
    l.len = (int)tb->partial.len;
    l.s = xstrndup(tb->partial.s ? tb->partial.s : "", tb->partial.len);
    l.pstart = tb->pstart;
    l.plen = tb->plen;
    if (tb->count == tb->cap) {
        free(tb->ring[tb->head].s);
        tb->head = (tb->head + 1) % tb->cap;
        tb->count--;
        tb->base++;
    }
    tb->ring[(tb->head + tb->count) % tb->cap] = l;
    tb->count++;
    buf_clear(&tb->partial);
    tb->col = 0;
    tb->pstart = tb->plen = 0;
}

static void tb_put(TermBuf *tb, char c)
{
    if ((size_t)tb->col < tb->partial.len) {
        tb->partial.s[tb->col] = c;
    } else {
        while (tb->partial.len < (size_t)tb->col)
            buf_putc(&tb->partial, ' ');
        buf_putc(&tb->partial, c);
    }
    tb->col++;
}

static void tb_csi(TermBuf *tb, char final)
{
    tb->csi[tb->csilen] = '\0';
    if (final == 'J' && (strcmp(tb->csi, "2") == 0 || strcmp(tb->csi, "3") == 0))
        tb_clear(tb); /* e.g. /usr/bin/clear */
    else if (final == 'K')
        tb->partial.len = (size_t)tb->col < tb->partial.len ? (size_t)tb->col : tb->partial.len;
    /* colours and cursor movement are ignored: output is plain text */
}

/* Feed program output: UTF-8 is mapped to Latin-1 for the core font,
 * escape sequences are swallowed, \r \b \t behave like a terminal. */
static void tb_feed(TermBuf *tb, const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)data[i];
        if (tb->esc == 1) {
            tb->esc = c == '[' ? 2 : c == ']' ? 3 : 0;
            tb->csilen = 0;
            continue;
        }
        if (tb->esc == 2) {
            if (c >= 0x40 && c <= 0x7e) {
                tb_csi(tb, (char)c);
                tb->esc = 0;
            } else if (tb->csilen < (int)sizeof tb->csi - 1) {
                tb->csi[tb->csilen++] = (char)c;
            }
            continue;
        }
        if (tb->esc == 3) {
            if (c == 7 || c == '\\')
                tb->esc = 0;
            continue;
        }
        if (tb->u8len > 0) {
            if ((c & 0xC0) == 0x80) {
                tb->u8[tb->u8len++] = c;
                unsigned cp;
                int used = utf8_decode(tb->u8, (size_t)tb->u8len, &cp);
                if (used < 0 && tb->u8len < 4)
                    continue; /* need more bytes */
                tb_put(tb, used > 0 && cp >= 0xA0 && cp <= 0xFF ? (char)cp : '?');
                tb->u8len = 0;
                continue;
            }
            tb_put(tb, '?'); /* truncated sequence; handle c normally */
            tb->u8len = 0;
        }
        if (c >= 0x80) {
            if ((c & 0xC0) == 0x80)
                tb_put(tb, '?');
            else {
                tb->u8[0] = c;
                tb->u8len = 1;
            }
            continue;
        }
        switch (c) {
        case 0x1b:
            tb->esc = 1;
            break;
        case '\n':
            tb_newline(tb);
            break;
        case '\r':
            tb->col = 0;
            break;
        case '\b':
            if (tb->col > 0)
                tb->col--;
            break;
        case '\t':
            do
                tb_put(tb, ' ');
            while (tb->col % 8);
            break;
        default:
            if (c >= 0x20 && c < 0x7f)
                tb_put(tb, (char)c);
            break;
        }
    }
}

static void tb_puts(TermBuf *tb, const char *s)
{
    tb_feed(tb, s, strlen(s));
}

/* UTF-8 -> display bytes; also maps a byte offset to a column. */
static void to_display(const char *s, size_t n, Buf *out, size_t cur_byte, int *cur_col)
{
    buf_clear(out);
    size_t i = 0;
    if (cur_col)
        *cur_col = -1;
    while (i < n) {
        if (cur_col && i >= cur_byte && *cur_col < 0)
            *cur_col = (int)out->len;
        unsigned cp;
        int used = utf8_decode((const unsigned char *)s + i, n - i, &cp);
        if (used <= 0)
            used = 1, cp = '?';
        if (cp == '\n')
            buf_putc(out, '\n');
        else if (cp == '\t')
            buf_putc(out, ' ');
        else if (cp < 0x20 || cp == 0x7f)
            buf_putc(out, '?');
        else
            buf_putc(out, cp <= 0xFF ? (char)cp : '?');
        i += (size_t)used;
    }
    if (cur_col && *cur_col < 0)
        *cur_col = (int)out->len;
}

/* ------------------------------------------------------------- colors */

static unsigned long color(const char *spec)
{
    XColor c;
    Colormap cm = DefaultColormap(dpy, screen);
    if (XParseColor(dpy, cm, spec, &c) && XAllocColor(dpy, cm, &c))
        return c.pixel;
    return WhitePixel(dpy, screen);
}

/* -------------------------------------------------------------- tabs */

static int visible_count(void)
{
    int n = 0;
    for (int i = 0; i < ntabs; i++)
        n += !tabs[i]->closing;
    return n;
}

static void set_window_title(void)
{
    char t[512];
    snprintf(t, sizeof t, "Terminator - %s%s", active && active->title ? active->title : "",
             process_mode ? "  [process mode]" : "");
    XStoreName(dpy, win, t);
}

static void select_tab(Tab *t)
{
    if (!t || t == active)
        return;
    active = t;
    sel_active = 0;
    set_window_title();
    dirty = 1;
}

static Tab *neighbour(Tab *t, int dir)
{
    int idx = -1;
    for (int i = 0; i < ntabs; i++)
        if (tabs[i] == t)
            idx = i;
    if (idx < 0)
        return NULL;
    for (int k = 1; k <= ntabs; k++) {
        Tab *c = tabs[((idx + dir * k) % ntabs + ntabs) % ntabs];
        if (!c->closing)
            return c;
    }
    return NULL;
}

static Tab *nth_visible(int n)
{
    for (int i = 0; i < ntabs; i++)
        if (!tabs[i]->closing && n-- == 0)
            return tabs[i];
    return NULL;
}

static void new_tab(void)
{
    if (ntabs >= MAX_TABS)
        return;
    int closefds[2 * MAX_TABS + 1], nclose = 0;
    closefds[nclose++] = ConnectionNumber(dpy);
    for (int i = 0; i < ntabs; i++) {
        closefds[nclose++] = tabs[i]->conn.to_shell;
        closefds[nclose++] = tabs[i]->conn.from_shell;
    }
    char *dir = NULL;
    if (active && active->title)
        dir = expand_home(active->title);
    Tab *t = xcalloc(1, sizeof *t);
    t->id = next_tab_id++;
    tb_init(&t->tb);
    buf_init(&t->edit_prompt);
    buf_init(&t->edit_text);

    // t->title = xstrdup(dir ? dir : "~");
// -------------------
    t->title = xstrdup("Terminal");
// ----------------------
    if (tabconn_spawn(&t->conn, process_mode, dir, closefds, nclose) < 0) {
        free(dir);
        tb_free(&t->tb);
        free(t->title);
        free(t);
        if (active)
            tb_puts(&active->tb, "terminator: could not start a new tab\n");
        return;
    }
    free(dir);
    tabconn_send_resize(&t->conn, cols, rows);
    tabs[ntabs++] = t;
    select_tab(t);
}

static void free_tab(Tab *t)
{
    tb_free(&t->tb);
    buf_free(&t->edit_prompt);
    buf_free(&t->edit_text);
    free(t->title);
    free(t);
}

static void remove_tab(Tab *t)
{
    Tab *next = (t == active) ? neighbour(t, 1) : active;
    if (next == t)
        next = NULL;
    for (int i = 0; i < ntabs; i++) {
        if (tabs[i] == t) {
            memmove(tabs + i, tabs + i + 1, (size_t)(ntabs - i - 1) * sizeof(Tab *));
            ntabs--;
            break;
        }
    }
    if (paste_target == t)
        paste_target = NULL;
    free_tab(t);
    active = NULL;
    select_tab(next);
    dirty = 1;
}

static void close_tab(Tab *t)
{
    if (!t || t->closing)
        return;
    if (t->dead) { /* already finished: just drop it */
        remove_tab(t);
        return;
    }
    t->closing = 1;
    tabconn_close_input(&t->conn); /* the shell sees EOF and exits */
    if (t == active) {
        Tab *n = neighbour(t, 1);
        active = NULL;
        select_tab(n && n != t ? n : NULL);
    }
    dirty = 1;
}

/* The shell side of a tab has gone away (EOF on its pipe). */
static void tab_ended(Tab *t)
{
    tabconn_finish(&t->conn);
    if (t->closing || t->conn.got_exit) {
        remove_tab(t);
        return;
    }
    /* process mode: the tab process crashed; the other tabs carry on */
    t->dead = 1;
    char msg[256];
    if (t->conn.crashed)
        snprintf(msg, sizeof msg, "\n[tab process terminated by signal %d (%s)]\n",
                 t->conn.crash_signal, strsignal(t->conn.crash_signal));
    else
        snprintf(msg, sizeof msg, "\n[the shell of this tab exited unexpectedly]\n");
    tb_puts(&t->tb, msg);
    tb_puts(&t->tb, "[press Ctrl+Shift+W to close this tab]\n");
    t->edit_visible = 0;
    dirty = 1;
}

static void show_errors(Tab *t, StrVec *errs)
{
    for (int i = 0; i < errs->n; i++) {
        fprintf(stderr, "terminator: %s\n", errs->v[i]);
        if (t) {
            tb_puts(&t->tb, "terminator: ");
            tb_puts(&t->tb, errs->v[i]);
            tb_puts(&t->tb, "\n");
        }
    }
    if (errs->n)
        dirty = 1;
}

static void reload_bindings(Tab *t)
{
    StrVec errs;
    sv_init(&errs);
    bindings_load(&kb, &errs);
    show_errors(t, &errs);
    sv_free(&errs);
}

static void handle_tab_message(Tab *t, uint32_t type, char *data, uint32_t len)
{
    switch (type) {
    case M_OUT:
        tb_feed(&t->tb, data, len);
        break;
    case M_COMMIT: {
        if (len < 4)
            break;
        int plen = msg_int(data, 0);
        t->tb.pstart = (int)t->tb.partial.len;
        t->tb.plen = plen; /* prompts are plain ASCII */
        tb_feed(&t->tb, data + 4, len - 4);
        tb_newline(&t->tb);
        break;
    }
    case M_EDIT: {
        if (len < 8)
            break;
        int plen = msg_int(data, 0);
        int cur = msg_int(data, 1);
        if (plen < 0) {
            t->edit_visible = 0;
            break;
        }
        const char *txt = data + 8;
        size_t tlen = len - 8;
        if ((size_t)plen > tlen)
            plen = (int)tlen;
        to_display(txt, (size_t)plen, &t->edit_prompt, 0, NULL);
        to_display(txt + plen, tlen - (size_t)plen, &t->edit_text, (size_t)cur, &t->edit_cursor);
        t->edit_visible = 1;
        if (t == active)
            t->scroll = 0;
        break;
    }
    case M_CLEAR:
        tb_clear(&t->tb);
        t->scroll = 0;
        if (t == active)
            sel_active = 0;
        break;
    // case M_TITLE:
    //     free(t->title);
    //     t->title = xstrndup(data, len);
    //     if (t == active)
    //         set_window_title();
    //     break;
// ----------------
    case M_TITLE:
            free(t->title);
            t->title = xstrdup("Terminal");
            if (t == active)
                set_window_title();
            break;
// ----------------
    case M_BINDRELOAD:
        reload_bindings(t);
        break;
    case M_EXIT:
        t->conn.got_exit = 1;
        t->conn.exit_status = len >= 4 ? msg_int(data, 0) : 0;
        break;
    default:
        break;
    }
    if (t == active || type == M_TITLE)
        dirty = 1;
}

static void read_tab(Tab *t)
{
    /* bounded, so a flood of output (`yes`) cannot starve X events */
    for (int rounds = 0; rounds < 16; rounds++) {
        long r = mr_fill(&t->conn.rd, t->conn.from_shell);
        uint32_t type, len;
        char *data;
        while (mr_next(&t->conn.rd, &type, &data, &len))
            handle_tab_message(t, type, data, len);
        if (r == 0) {
            tab_ended(t);
            return;
        }
        if (r < 0)
            return;
    }
}

/* ------------------------------------------------------------ layout */

static void compute_grid(void)
{
    int ncols = (win_w - 2 * PAD) / cw;
    int nrows = (win_h - bar_h - PAD) / ch;
    if (ncols < 10)
        ncols = 10;
    if (nrows < 2)
        nrows = 2;
    if (ncols != cols || nrows != rows) {
        cols = ncols;
        rows = nrows;
        for (int i = 0; i < ntabs; i++)
            tabconn_send_resize(&tabs[i]->conn, cols, rows);
    }
}

static void build_bottom(Tab *t)
{
    nbottom = 0;
    TermBuf *tb = &t->tb;
    long abs0 = tb->base + tb->count;
    if (!t->edit_visible) {
        if (tb->partial.len > 0) {
            buf_clear(&bottom_store[0]);
            buf_putn(&bottom_store[0], tb->partial.s, tb->partial.len);
            bottom[0] = (LLine){ bottom_store[0].s, (int)tb->partial.len, tb->pstart, tb->plen, -1,
                                 abs0 };
            nbottom = 1;
        }
        return;
    }
    const char *txt = t->edit_text.s ? t->edit_text.s : "";
    int tlen = (int)t->edit_text.len;
    int start = 0, k = 0;
    for (int i = 0; i <= tlen && k < 64; i++) {
        if (i < tlen && txt[i] != '\n')
            continue;
        Buf *b = &bottom_store[k];
        buf_clear(b);
        int pstart = 0, plen = 0, base = 0;
        if (k == 0) {
            buf_putn(b, tb->partial.s ? tb->partial.s : "", tb->partial.len);
            if (tb->plen) {
                pstart = tb->pstart;
                plen = tb->plen;
            }
            if (t->edit_prompt.len) {
                pstart = (int)b->len;
                plen = (int)t->edit_prompt.len;
            }
            buf_putn(b, t->edit_prompt.s ? t->edit_prompt.s : "", t->edit_prompt.len);
            base = (int)b->len;
        }
        buf_putn(b, txt + start, (size_t)(i - start));
        int cursor = -1;
        if (t->edit_cursor >= start && t->edit_cursor <= i)
            cursor = base + t->edit_cursor - start;
        bottom[k] = (LLine){ b->s ? b->s : "", (int)b->len, pstart, plen, cursor, abs0 + k };
        k++;
        start = i + 1;
    }
    nbottom = k;
}

static void push_rows(const LLine *l, int want)
{
    int need = l->len;
    if (l->cursor >= 0 && l->cursor + 1 > need)
        need = l->cursor + 1;
    int nr = need == 0 ? 1 : (need + cols - 1) / cols;
    for (int r = nr - 1; r >= 0 && frame_nrows < want; r--) {
        if (frame_nrows >= frame_cap) {
            frame_cap = frame_cap ? frame_cap * 2 : 256;
            frame_rows = xrealloc(frame_rows, (size_t)frame_cap * sizeof(VRow));
        }
        VRow *v = &frame_rows[frame_nrows++];
        v->s = l->s;
        v->off = r * cols;
        v->len = l->len - v->off;
        if (v->len > cols)
            v->len = cols;
        if (v->len < 0)
            v->len = 0;
        v->pstart = l->pstart;
        v->plen = l->plen;
        v->cursor = (l->cursor >= r * cols && l->cursor < (r + 1) * cols) ? l->cursor - r * cols : -1;
        v->abs = l->abs;
    }
}

static void collect_rows(Tab *t, int want)
{
    frame_nrows = 0;
    build_bottom(t);
    for (int k = nbottom - 1; k >= 0 && frame_nrows < want; k--)
        push_rows(&bottom[k], want);
    TermBuf *tb = &t->tb;
    for (int i = tb->count - 1; i >= 0 && frame_nrows < want; i--) {
        Line *l = tb_line(tb, i);
        LLine ll = { l->s, l->len, l->pstart, l->plen, -1, tb->base + i };
        push_rows(&ll, want);
    }
}

/* ---------------------------------------------------------- selection */

static void sel_order(long *a_abs, int *a_off, long *b_abs, int *b_off)
{
    *a_abs = sel_a_abs;
    *a_off = sel_a_off;
    *b_abs = sel_b_abs;
    *b_off = sel_b_off;
    if (*a_abs > *b_abs || (*a_abs == *b_abs && *a_off > *b_off)) {
        long t = *a_abs;
        *a_abs = *b_abs;
        *b_abs = t;
        int o = *a_off;
        *a_off = *b_off;
        *b_off = o;
    }
}

static const char *line_by_abs(Tab *t, long abs, int *len)
{
    TermBuf *tb = &t->tb;
    if (abs >= tb->base && abs < tb->base + tb->count) {
        Line *l = tb_line(tb, (int)(abs - tb->base));
        *len = l->len;
        return l->s;
    }
    long k = abs - (tb->base + tb->count);
    if (k >= 0 && k < nbottom) {
        *len = bottom[k].len;
        return bottom[k].s;
    }
    return NULL;
}

static char *selection_text(Tab *t)
{
    if (!sel_active || !t)
        return NULL;
    long a, b;
    int ao, bo;
    sel_order(&a, &ao, &b, &bo);
    if (a == b && ao == bo)
        return NULL;
    Buf raw, out;
    buf_init(&raw);
    buf_init(&out);
    for (long abs = a; abs <= b; abs++) {
        int len;
        const char *s = line_by_abs(t, abs, &len);
        if (!s)
            continue;
        int from = abs == a ? ao : 0;
        int to = abs == b ? bo : len;
        if (from > len)
            from = len;
        if (to > len)
            to = len;
        if (to > from)
            buf_putn(&raw, s + from, (size_t)(to - from));
        if (abs != b)
            buf_putc(&raw, '\n');
    }
    latin1_to_utf8(&out, raw.s ? raw.s : "", raw.len);
    buf_free(&raw);
    return buf_detach(&out);
}

static int pos_from_xy(int x, int y, long *abs, int *off)
{
    if (!active || frame_nrows == 0)
        return 0;
    int text_rows = rows;
    int sr = (y - bar_h) / ch;
    if (sr < 0)
        sr = 0;
    if (sr >= text_rows)
        sr = text_rows - 1;
    int scroll = active->scroll;
    int k = text_rows - 1 - sr + scroll;
    if (k >= frame_nrows)
        k = frame_nrows - 1;
    if (k < 0)
        k = 0;
    VRow *v = &frame_rows[k];
    int c = (x - PAD + cw / 2) / cw;
    if (c < 0)
        c = 0;
    if (c > v->len)
        c = v->len;
    *abs = v->abs;
    *off = v->off + c;
    return 1;
}

static void own_selection(Atom which, const char *text, Time when)
{
    if (!text)
        return;
    if (which == XA_PRIMARY) {
        free(primary_text);
        primary_text = xstrdup(text);
    } else {
        free(clip_text);
        clip_text = xstrdup(text);
    }
    XSetSelectionOwner(dpy, which, win, when);
}

static void utf8_to_latin1(Buf *out, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned cp;
        int used = utf8_decode((const unsigned char *)s + i, n - i, &cp);
        if (used <= 0)
            used = 1, cp = '?';
        buf_putc(out, cp <= 0xFF ? (char)cp : '?');
        i += (size_t)used;
    }
}

static void serve_selection(XSelectionRequestEvent *rq)
{
    XSelectionEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SelectionNotify;
    ev.display = rq->display;
    ev.requestor = rq->requestor;
    ev.selection = rq->selection;
    ev.target = rq->target;
    ev.time = rq->time;
    ev.property = None;
    Atom prop = rq->property != None ? rq->property : rq->target;
    const char *text = rq->selection == a_clipboard ? clip_text : primary_text;
    if (text) {
        if (rq->target == a_targets) {
            Atom list[4] = { a_targets, a_utf8, XA_STRING, a_text };
            XChangeProperty(dpy, rq->requestor, prop, XA_ATOM, 32, PropModeReplace,
                            (unsigned char *)list, 4);
            ev.property = prop;
        } else if (rq->target == a_utf8 || rq->target == a_text) {
            XChangeProperty(dpy, rq->requestor, prop, a_utf8, 8, PropModeReplace,
                            (const unsigned char *)text, (int)strlen(text));
            ev.property = prop;
        } else if (rq->target == XA_STRING) {
            Buf l1;
            buf_init(&l1);
            utf8_to_latin1(&l1, text, strlen(text));
            XChangeProperty(dpy, rq->requestor, prop, XA_STRING, 8, PropModeReplace,
                            (const unsigned char *)(l1.s ? l1.s : ""), (int)l1.len);
            buf_free(&l1);
            ev.property = prop;
        }
    }
    XSendEvent(dpy, rq->requestor, False, 0, (XEvent *)&ev);
}

/* Pasted text becomes typed input: each newline presses Enter. */
static void paste_into(Tab *t, const char *s, size_t n)
{
    if (!t || t->dead || t->closing)
        return;
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && s[i] != '\n' && s[i] != '\r')
            continue;
        if (i > start)
            tabconn_send_key(&t->conn, ACT_INSERT, s + start, i - start);
        if (i < n) {
            tabconn_send_key(&t->conn, ACT_ENTER, NULL, 0);
            if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n')
                i++;
        }
        start = i + 1;
    }
    t->scroll = 0;
}

static void request_paste(Atom which, Time when)
{
    if (!active)
        return;
    Window owner = XGetSelectionOwner(dpy, which);
    if (owner == win) {
        const char *text = which == a_clipboard ? clip_text : primary_text;
        if (text)
            paste_into(active, text, strlen(text));
        return;
    }
    if (owner == None)
        return;
    paste_target = active;
    paste_target_type = a_utf8;
    XConvertSelection(dpy, which, a_utf8, a_paste_prop, win, when);
}

static void got_selection(XSelectionEvent *ev)
{
    if (ev->property == None) {
        if (paste_target_type == a_utf8) { /* fall back to Latin-1 STRING */
            paste_target_type = XA_STRING;
            XConvertSelection(dpy, ev->selection, XA_STRING, a_paste_prop, win, ev->time);
        }
        return;
    }
    Atom type;
    int format;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, ev->property, 0, 1 << 22, True, AnyPropertyType, &type, &format,
                           &nitems, &after, &data) == Success && data) {
        if (format == 8) {
            if (type == XA_STRING) {
                Buf u;
                buf_init(&u);
                latin1_to_utf8(&u, (const char *)data, nitems);
                paste_into(paste_target, u.s ? u.s : "", u.len);
                buf_free(&u);
            } else {
                paste_into(paste_target, (const char *)data, nitems);
            }
        }
        XFree(data);
    }
    dirty = 1;
}

/* ------------------------------------------------------------ drawing */

static void draw_text(int x, int y, const char *s, int n, unsigned long fg)
{
    if (n <= 0)
        return;
    XSetForeground(dpy, gc, fg);
    XDrawString(dpy, back, gc, x, y + ascent, s, n);
}

static void draw_row(const VRow *v, int y, long sa, int sao, long sb, int sbo)
{
    const char *s = v->s + v->off;
    /* selection highlight */
    if (sel_active && v->abs >= sa && v->abs <= sb && v->len > 0) {
        int from = v->abs == sa ? sao : 0;
        int to = v->abs == sb ? sbo : 1 << 30;
        int a = from - v->off, b = to - v->off;
        if (a < 0)
            a = 0;
        if (b > v->len)
            b = v->len;
        if (v->abs != sb && b < v->len)
            b = v->len;
        if (b > a) {
            XSetForeground(dpy, gc, col_sel);
            XFillRectangle(dpy, back, gc, PAD + a * cw, y, (unsigned)((b - a) * cw), (unsigned)ch);
        }
    }
    /* prompt part and the rest */
    int p0 = v->pstart - v->off, p1 = v->pstart + v->plen - v->off;
    if (p0 < 0)
        p0 = 0;
    if (p1 > v->len)
        p1 = v->len;
    if (v->plen > 0 && p1 > p0) {
        draw_text(PAD, y, s, p0, col_fg);
        draw_text(PAD + p0 * cw, y, s + p0, p1 - p0, col_prompt);
        draw_text(PAD + p1 * cw, y, s + p1, v->len - p1, col_fg);
    } else {
        draw_text(PAD, y, s, v->len, col_fg);
    }
    /* cursor */
    if (v->cursor >= 0) {
        int cx = PAD + v->cursor * cw;
        XSetForeground(dpy, gc, col_cursor);
        if (focused) {
            XFillRectangle(dpy, back, gc, cx, y, (unsigned)cw, (unsigned)ch);
            if (v->cursor < v->len)
                draw_text(cx, y, s + v->cursor, 1, col_bg);
        } else {
            XDrawRectangle(dpy, back, gc, cx, y, (unsigned)cw - 1, (unsigned)ch - 1);
        }
    }
}

// static void draw_tab_bar(void)
// {
//     XSetForeground(dpy, gc, col_bar);
//     XFillRectangle(dpy, back, gc, 0, 0, (unsigned)win_w, (unsigned)bar_h - 2);
//     int x = PAD;
//     int y = (bar_h - 2 - ch) / 2;
//     int pos = 0;
//     for (int i = 0; i < ntabs; i++) {
//         Tab *t = tabs[i];
//         if (t->closing)
//             continue;
//         pos++;
//         const char *title = t->title ? t->title : "";
//         char shown[64];
//         size_t tl = strlen(title);
//         if (tl > 24)
//             snprintf(shown, sizeof shown, "..%s", title + tl - 22);
//         else
//             snprintf(shown, sizeof shown, "%s", title);
//         char label[96];
//         int n = snprintf(label, sizeof label, "[ %d: %s%s ]", pos, shown, t->dead ? " (dead)" : "");
//         int w = n * cw;
//         XSetForeground(dpy, gc, t == active ? col_tab_active : col_tab);
//         XFillRectangle(dpy, back, gc, x - 3, 2, (unsigned)w + 6, (unsigned)bar_h - 6);
//         draw_text(x, y, label, n, t->dead ? col_dead : t == active ? col_tab_text_active : col_tab_text);
//         t->bar_x0 = x - 3;
//         t->bar_x1 = x + w + 3;
//         x += w + cw + 6;
//     }
//     const char *plus = "[ + ]";
//     int pw = (int)strlen(plus) * cw;
//     XSetForeground(dpy, gc, col_tab);
//     XFillRectangle(dpy, back, gc, x - 3, 2, (unsigned)pw + 6, (unsigned)bar_h - 6);
//     draw_text(x, y, plus, (int)strlen(plus), col_tab_text);
//     plus_x0 = x - 3;
//     plus_x1 = x + pw + 3;
//     XSetForeground(dpy, gc, col_tab_active);
//     XFillRectangle(dpy, back, gc, 0, bar_h - 2, (unsigned)win_w, 2);
// }

static void draw_tab_bar(void)
{
    XSetForeground(dpy, gc, col_bar);
    XFillRectangle(dpy, back, gc, 0, 0, (unsigned)win_w, (unsigned)bar_h - 2);

    int x = PAD;
    int y = (bar_h - 2 - ch) / 2;
    int pos = 0;

    for (int i = 0; i < ntabs; i++) {
        Tab *t = tabs[i];

        if (t->closing)
            continue;

        pos++;

        const char *title = t->title ? t->title : "";
        char shown[64];

        size_t tl = strlen(title);

        if (tl > 24)
            snprintf(shown, sizeof shown, "..%s", title + tl - 22);
        else
            snprintf(shown, sizeof shown, "%s", title);

        char label[96];

        int n = snprintf(label, sizeof label, "[ %d: %s%s ]",
                         pos, shown, t->dead ? " (dead)" : "");

        int w = n * cw;

        XSetForeground(dpy, gc, t == active ? col_tab_active : col_tab);

        XFillRectangle(dpy, back, gc, x - 3, 2,
                       (unsigned)w + (unsigned)cw + 6,
                       (unsigned)bar_h - 6);

        draw_text(x, y, label, n,
                  t->dead ? col_dead :
                  t == active ? col_tab_text_active : col_tab_text);

        // Draw the clear button
        draw_text(x + w, y, "x", 1,
                  t == active ? col_tab_text_active : col_tab_text);

        t->bar_x0 = x - 3;
        t->bar_x1 = x + w + cw + 3;

        t->clear_x0 = x + w - 2;
        t->clear_x1 = x + w + cw + 2;

        x += w + 2 * cw + 6;
    }

    const char *plus = "[ + ]";
    int pw = (int)strlen(plus) * cw;

    XSetForeground(dpy, gc, col_tab);

    XFillRectangle(dpy, back, gc, x - 3, 2,
                   (unsigned)pw + 6, (unsigned)bar_h - 6);

    draw_text(x, y, plus, (int)strlen(plus), col_tab_text);

    plus_x0 = x - 3;
    plus_x1 = x + pw + 3;

    XSetForeground(dpy, gc, col_tab_active);
    XFillRectangle(dpy, back, gc, 0, bar_h - 2,
                   (unsigned)win_w, 2);
}

static void redraw(void)
{
    XSetForeground(dpy, gc, col_bg);
    XFillRectangle(dpy, back, gc, 0, 0, (unsigned)win_w, (unsigned)win_h);
    draw_tab_bar();
    if (active) {
        int want = active->scroll + rows;
        collect_rows(active, want);
        if (active->scroll > 0 && frame_nrows < want) {
            active->scroll = frame_nrows > rows ? frame_nrows - rows : 0;
            collect_rows(active, active->scroll + rows);
        }
        long sa = 0, sb = 0;
        int sao = 0, sbo = 0;
        if (sel_active)
            sel_order(&sa, &sao, &sb, &sbo);
        for (int k = 0; k < rows; k++) {
            int idx = active->scroll + k;
            if (idx >= frame_nrows)
                break;
            int y = bar_h + (rows - 1 - k) * ch;
            draw_row(&frame_rows[idx], y, sa, sao, sb, sbo);
        }
        if (active->scroll > 0) {
            char ind[48];
            int n = snprintf(ind, sizeof ind, "[scrolled back %d]", active->scroll);
            XSetForeground(dpy, gc, col_tab);
            XFillRectangle(dpy, back, gc, win_w - PAD - n * cw - 4, bar_h, (unsigned)(n * cw + 4),
                           (unsigned)ch);
            draw_text(win_w - PAD - n * cw - 2, bar_h, ind, n, col_tab_text_active);
        }
    }
    XCopyArea(dpy, back, win, gc, 0, 0, (unsigned)win_w, (unsigned)win_h, 0, 0);
}

/* --------------------------------------------------------------- input */

static void scroll_by(int delta)
{
    if (!active)
        return;
    active->scroll += delta;
    if (active->scroll < 0)
        active->scroll = 0;
    dirty = 1;
}

static void do_action(int a, Time when)
{
    switch (a) {
    case KA_COPY: {
        char *text = selection_text(active);
        if (!text && active) { /* nothing selected: copy the input line */
            Buf u;
            buf_init(&u);
            latin1_to_utf8(&u, active->edit_text.s ? active->edit_text.s : "", active->edit_text.len);
            text = buf_detach(&u);
        }
        if (text && *text)
            own_selection(a_clipboard, text, when);
        free(text);
        return;
    }
    case KA_PASTE:
        request_paste(a_clipboard, when);
        return;
    case KA_NEW_TAB:
        new_tab();
        return;
    case KA_CLOSE_TAB:
        close_tab(active);
        return;
    case KA_NEXT_TAB:
        select_tab(neighbour(active, 1));
        return;
    case KA_PREV_TAB:
        select_tab(neighbour(active, -1));
        return;
    case KA_SCROLL_UP:
        scroll_by(rows - 1);
        return;
    case KA_SCROLL_DOWN:
        scroll_by(-(rows - 1));
        return;
    default:
        break;
    }
    int act = keyaction_shell_action(a);
    if (act && active && !active->dead) {
        tabconn_send_key(&active->conn, act, NULL, 0);
        active->scroll = 0;
        dirty = 1;
    }
}

static void send_action(int act)
{
    if (active && !active->dead) {
        tabconn_send_key(&active->conn, act, NULL, 0);
        active->scroll = 0;
        dirty = 1;
    }
}

static void handle_key(XKeyEvent *e)
{
    char buf[64];
    KeySym ks = NoSymbol;
    int n = XLookupString(e, buf, sizeof buf, &ks, NULL);
    unsigned mods = 0;
    if (e->state & ShiftMask)
        mods |= KMOD_SHIFT;
    if (e->state & ControlMask)
        mods |= KMOD_CTRL;
    if (e->state & Mod1Mask)
        mods |= KMOD_ALT;
    if (ks == XK_ISO_Left_Tab)
        mods |= KMOD_SHIFT;
    if (ks == XK_Shift_L || ks == XK_Shift_R || ks == XK_Control_L || ks == XK_Control_R ||
        ks == XK_Alt_L || ks == XK_Alt_R || ks == XK_Meta_L || ks == XK_Meta_R ||
        ks == XK_Caps_Lock || ks == XK_Super_L || ks == XK_Super_R)
        return;

    KeyCombo kc = { mods, key_normalise_sym(ks) };
    int a = bindings_lookup(&kb, &kc);
    if (a >= 0) {
        do_action(a, e->time);
        return;
    }

    /* fixed keys */
    if ((mods & KMOD_ALT) && ks >= XK_1 && ks <= XK_9) {
        select_tab(nth_visible((int)(ks - XK_1)));
        return;
    }
    if ((mods & KMOD_CTRL) && kc.sym == XK_Tab) {
        select_tab(neighbour(active, (mods & KMOD_SHIFT) ? -1 : 1));
        return;
    }
    if ((mods & KMOD_SHIFT) && ks == XK_Insert) {
        request_paste(XA_PRIMARY, e->time);
        return;
    }
    switch (ks) {
    case XK_Return:
    case XK_KP_Enter:
        send_action(ACT_ENTER);
        return;
    case XK_BackSpace:
        send_action(ACT_BACKSPACE);
        return;
    case XK_Delete:
    case XK_KP_Delete:
        send_action(ACT_DELETE);
        return;
    case XK_Home:
    case XK_KP_Home:
        send_action(ACT_HOME);
        return;
    case XK_End:
    case XK_KP_End:
        send_action(ACT_END);
        return;
    case XK_Escape:
        send_action(ACT_ESCAPE);
        return;
    default:
        break;
    }
    if (mods & (KMOD_CTRL | KMOD_ALT))
        return;
    if (n > 0 && active && !active->dead) {
        Buf u;
        buf_init(&u);
        for (int i = 0; i < n; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (c >= 0x20 && c != 0x7f)
                latin1_to_utf8(&u, (char *)&c, 1);
        }
        if (u.len) {
            tabconn_send_key(&active->conn, ACT_INSERT, u.s, u.len);
            active->scroll = 0;
            dirty = 1;
        }
        buf_free(&u);
    }
}

static void handle_button(XButtonEvent *e, int press)
{
    if (e->y < bar_h) {
        if (!press)
            return;
        if (e->button == Button1 && e->x >= plus_x0 && e->x < plus_x1) {
            new_tab();
            return;
        }
        for (int i = 0; i < ntabs; i++) {
            Tab *t = tabs[i];
            if (t->closing || e->x < t->bar_x0 || e->x >= t->bar_x1)
                continue;
            // if (e->button == Button1)
            //     select_tab(t);
            // else if (e->button == Button2 || e->button == Button3)
            //     close_tab(t);
            // return;
            // Add x
            if (e->button == Button1) {
                if (e->x >= t->clear_x0 && e->x < t->clear_x1) {
                    // Clear only this tab's terminal contents
                    tb_clear(&t->tb);

                    t->scroll = 0;

                    if (t == active)
                        sel_active = 0;

                    dirty = 1;
                }
                else {
                    select_tab(t);
                }
            }
            else if (e->button == Button2 || e->button == Button3) {
                close_tab(t);
            }

            return;
        }
        if (e->button == Button4)
            select_tab(neighbour(active, -1));
        if (e->button == Button5)
            select_tab(neighbour(active, 1));
        return;
    }
    switch (e->button) {
    case Button1:
        if (press) {
            if (pos_from_xy(e->x, e->y, &sel_a_abs, &sel_a_off)) {
                sel_b_abs = sel_a_abs;
                sel_b_off = sel_a_off;
                sel_active = 1;
                sel_dragging = 1;
            }
        } else if (sel_dragging) {
            sel_dragging = 0;
            pos_from_xy(e->x, e->y, &sel_b_abs, &sel_b_off);
            char *text = selection_text(active);
            if (text && *text)
                own_selection(XA_PRIMARY, text, e->time);
            else
                sel_active = 0;
            free(text);
        }
        dirty = 1;
        break;
    case Button2:
        if (press)
            request_paste(XA_PRIMARY, e->time);
        break;
    case Button4:
        if (press)
            scroll_by(3);
        break;
    case Button5:
        if (press)
            scroll_by(-3);
        break;
    default:
        break;
    }
}

static void handle_event(XEvent *ev)
{
    switch (ev->type) {
    case Expose:
        if (ev->xexpose.count == 0)
            dirty = 1;
        break;
    case ConfigureNotify:
        if (ev->xconfigure.width != win_w || ev->xconfigure.height != win_h) {
            win_w = ev->xconfigure.width;
            win_h = ev->xconfigure.height;
            XFreePixmap(dpy, back);
            back = XCreatePixmap(dpy, win, (unsigned)win_w, (unsigned)win_h,
                                 (unsigned)DefaultDepth(dpy, screen));
            compute_grid();
            dirty = 1;
        }
        break;
    case KeyPress:
        handle_key(&ev->xkey);
        break;
    case ButtonPress:
        handle_button(&ev->xbutton, 1);
        break;
    case ButtonRelease:
        handle_button(&ev->xbutton, 0);
        break;
    case MotionNotify:
        if (sel_dragging) {
            pos_from_xy(ev->xmotion.x, ev->xmotion.y, &sel_b_abs, &sel_b_off);
            dirty = 1;
        }
        break;
    case FocusIn:
        focused = 1;
        dirty = 1;
        break;
    case FocusOut:
        focused = 0;
        dirty = 1;
        break;
    case SelectionRequest:
        serve_selection(&ev->xselectionrequest);
        break;
    case SelectionNotify:
        got_selection(&ev->xselection);
        break;
    case SelectionClear:
        if (ev->xselectionclear.selection == XA_PRIMARY) {
            sel_active = 0;
            dirty = 1;
        }
        break;
    case ClientMessage:
        if (ev->xclient.message_type == a_wm_protocols &&
            (Atom)ev->xclient.data.l[0] == a_wm_delete) {
            for (int i = ntabs - 1; i >= 0; i--) /* close_tab may remove */
                close_tab(tabs[i]);
        }
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------- setup */

static int open_window(void)
{
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "terminator: cannot open X display '%s'\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "");
        return -1;
    }
    set_cloexec(ConnectionNumber(dpy), 1);
    screen = DefaultScreen(dpy);

    const char *want = getenv("TERMINATOR_FONT");
    const char *fonts[] = { want, "-misc-fixed-medium-r-normal--18-120-100-100-c-90-iso8859-1",
                            "9x15", "-*-fixed-medium-r-normal--15-*-*-*-*-*-iso8859-1", "fixed" };
    for (size_t i = 0; i < ARRAY_LEN(fonts) && !font; i++)
        if (fonts[i])
            font = XLoadQueryFont(dpy, fonts[i]);
    if (!font) {
        fprintf(stderr, "terminator: cannot load a fixed-width font\n");
        return -1;
    }
    cw = font->max_bounds.width;
    ascent = font->ascent;
    ch = font->ascent + font->descent + 1;
    bar_h = ch + 10;

    col_bg = color("#1d1f27");
    col_fg = color("#d8dee9");
    col_prompt = color("#8fd16a");
    col_bar = color("#14161c");
    col_tab = color("#2e3440");
    col_tab_active = color("#4c7899");
    col_tab_text = color("#aab2c0");
    col_tab_text_active = color("#ffffff");
    col_cursor = color("#f0c674");
    col_sel = color("#44506a");
    col_dead = color("#e06c75");

    win_w = 2 * PAD + 100 * cw;
    win_h = bar_h + 32 * ch + PAD;
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, (unsigned)win_w, (unsigned)win_h, 0,
                              col_bg, col_bg);
    XSelectInput(dpy, win,
                 ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                     Button1MotionMask | StructureNotifyMask | FocusChangeMask);

    XSizeHints *sz = XAllocSizeHints();
    if (sz) {
        sz->flags = PResizeInc | PMinSize | PBaseSize;
        sz->width_inc = cw;
        sz->height_inc = ch;
        sz->base_width = 2 * PAD;
        sz->base_height = bar_h + PAD;
        sz->min_width = 2 * PAD + 20 * cw;
        sz->min_height = bar_h + PAD + 4 * ch;
        XSetWMNormalHints(dpy, win, sz);
        XFree(sz);
    }
    XClassHint *cls = XAllocClassHint();
    if (cls) {
        cls->res_name = "terminator";
        cls->res_class = "Terminator";
        XSetClassHint(dpy, win, cls);
        XFree(cls);
    }

    a_wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    a_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    a_targets = XInternAtom(dpy, "TARGETS", False);
    a_text = XInternAtom(dpy, "TEXT", False);
    a_paste_prop = XInternAtom(dpy, "TERMINATOR_PASTE", False);
    XSetWMProtocols(dpy, win, &a_wm_delete, 1);

    gc = XCreateGC(dpy, win, 0, NULL);
    XSetFont(dpy, gc, font->fid);
    back = XCreatePixmap(dpy, win, (unsigned)win_w, (unsigned)win_h,
                         (unsigned)DefaultDepth(dpy, screen));
    XStoreName(dpy, win, "Terminator");
    XMapWindow(dpy, win);
    cols = (win_w - 2 * PAD) / cw;
    rows = (win_h - bar_h - PAD) / ch;
    return 0;
}

int gui_run(int pm)
{
    process_mode = pm;
    if (open_window() < 0)
        return 1;
    for (size_t i = 0; i < ARRAY_LEN(bottom_store); i++)
        buf_init(&bottom_store[i]);

    new_tab();
    if (!active) {
        fprintf(stderr, "terminator: could not start the first tab\n");
        return 1;
    }
    reload_bindings(active);

    struct pollfd pfd[1 + 2 * MAX_TABS];
    Tab *pfd_tab[1 + 2 * MAX_TABS];
    for (;;) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            handle_event(&ev);
        }
        if (visible_count() == 0 && !quitting) {
            quitting = 1;
            quit_started = now_ms();
            for (int i = ntabs - 1; i >= 0; i--) /* close_tab may remove */
                close_tab(tabs[i]);
        }
        if (quitting && (ntabs == 0 || now_ms() - quit_started > 3000))
            break;
        if (dirty) {
            redraw();
            dirty = 0;
        }
        if (XPending(dpy)) /* flushes, and catches events read meanwhile */
            continue;

        int n = 0;
        pfd[n].fd = ConnectionNumber(dpy);
        pfd[n].events = POLLIN;
        pfd_tab[n++] = NULL;
        for (int i = 0; i < ntabs; i++) {
            Tab *t = tabs[i];
            if (t->conn.from_shell >= 0) {
                pfd[n].fd = t->conn.from_shell;
                pfd[n].events = POLLIN;
                pfd_tab[n++] = t;
            }
            if (t->conn.txq.len && t->conn.to_shell >= 0) {
                pfd[n].fd = t->conn.to_shell;
                pfd[n].events = POLLOUT;
                pfd_tab[n++] = t;
            }
        }
        int r = poll(pfd, (nfds_t)n, quitting ? 200 : -1);
        if (r < 0 && errno != EINTR)
            break;
        for (int k = 1; k < n && r > 0; k++) {
            if (!pfd[k].revents)
                continue;
            Tab *t = pfd_tab[k];
            int still = 0;
            for (int i = 0; i < ntabs; i++)
                still |= tabs[i] == t;
            if (!still)
                continue; /* removed while handling an earlier fd */
            if (pfd[k].events & POLLOUT)
                tabconn_flush(&t->conn);
            else
                read_tab(t);
        }
    }

    while (ntabs > 0) { /* timed out waiting: make sure nothing is left */
        Tab *t = tabs[0];
        if (t->conn.pid > 0)
            kill(t->conn.pid, SIGKILL);
        if (t->conn.has_thread)
            pthread_detach(t->conn.thread), t->conn.has_thread = 0;
        tabconn_finish(&t->conn);
        tabs[0] = tabs[--ntabs];
        free_tab(t);
    }
    XFreePixmap(dpy, back);
    XFreeGC(dpy, gc);
    XFreeFont(dpy, font);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
