/*
 * parse.c - lexer and parser (see parse.h).
 */
#include "parse.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ scanner */

int is_name_start(int c)
{
    return c == '_' || isalpha((unsigned char)c);
}

int is_name_char(int c)
{
    return c == '_' || isalnum((unsigned char)c);
}

size_t assignment_name_len(const char *w)
{
    if (!is_name_start(w[0]))
        return 0;
    size_t i = 1;
    while (is_name_char(w[i]))
        i++;
    return w[i] == '=' ? i : 0;
}

static long scan_squote(const char *s, size_t i, size_t n)
{
    /* s[i] == '\'' */
    for (size_t j = i + 1; j < n; j++)
        if (s[j] == '\'')
            return (long)j + 1;
    return -1;
}

static long scan_dquote(const char *s, size_t i, size_t n)
{
    /* s[i] == '"' */
    size_t j = i + 1;
    while (j < n) {
        char c = s[j];
        if (c == '"')
            return (long)j + 1;
        if (c == '\\') {
            if (j + 1 >= n)
                return -1;
            j += 2;
        } else if (c == '$' && j + 1 < n && s[j + 1] == '(') {
            long e = scan_subst(s, j, n);
            if (e < 0)
                return -1;
            j = (size_t)e;
        } else if (c == '$' && j + 1 < n && s[j + 1] == '{') {
            long e = scan_brace(s, j, n);
            if (e < 0)
                return -1;
            j = (size_t)e;
        } else if (c == '`') {
            long e = scan_backtick(s, j, n);
            if (e < 0)
                return -1;
            j = (size_t)e;
        } else {
            j++;
        }
    }
    return -1;
}

long scan_backtick(const char *s, size_t i, size_t n)
{
    size_t j = i + 1;
    while (j < n) {
        if (s[j] == '\\') {
            j += 2;
            continue;
        }
        if (s[j] == '`')
            return (long)j + 1;
        j++;
    }
    return -1;
}

long scan_brace(const char *s, size_t i, size_t n)
{
    /* s[i] == '$', s[i+1] == '{' */
    for (size_t j = i + 2; j < n; j++)
        if (s[j] == '}')
            return (long)j + 1;
    return -1;
}

long scan_subst(const char *s, size_t i, size_t n)
{
    /* s[i] == '$', s[i+1] == '(' : find the matching ')' while honouring
     * quotes, nested substitutions and comments inside. */
    int depth = 1;
    size_t j = i + 2;
    int word_start = 1;
    while (j < n) {
        char c = s[j];
        long e;
        switch (c) {
        case '\\':
            if (j + 1 >= n)
                return -1;
            j += 2;
            word_start = 0;
            continue;
        case '\'':
            if ((e = scan_squote(s, j, n)) < 0)
                return -1;
            j = (size_t)e;
            word_start = 0;
            continue;
        case '"':
            if ((e = scan_dquote(s, j, n)) < 0)
                return -1;
            j = (size_t)e;
            word_start = 0;
            continue;
        case '`':
            if ((e = scan_backtick(s, j, n)) < 0)
                return -1;
            j = (size_t)e;
            word_start = 0;
            continue;
        case '$':
            if (j + 1 < n && s[j + 1] == '(') {
                if ((e = scan_subst(s, j, n)) < 0)
                    return -1;
                j = (size_t)e;
                word_start = 0;
                continue;
            }
            break;
        case '#':
            if (word_start) {
                while (j < n && s[j] != '\n')
                    j++;
                continue;
            }
            break;
        case '(':
            depth++;
            break;
        case ')':
            if (--depth == 0)
                return (long)j + 1;
            break;
        default:
            break;
        }
        word_start = (c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '|' || c == '&' ||
                      c == '(');
        j++;
    }
    return -1;
}

/* ================================================================== lexer */

typedef enum {
    T_WORD,
    T_NL,
    T_SEMI,
    T_AMP,
    T_PIPE,
    T_ANDAND,
    T_OROR,
    T_REDIR,
    T_LPAREN,
    T_RPAREN,
    T_DSEMI,
    T_EOF
} TokType;

typedef struct {
    TokType type;
    char *text;
    int line;
    size_t start, end;
    RedirType rtype;
    int rfd;
} Tok;

typedef struct {
    Tok *v;
    int n, cap;
} TokVec;

static void tv_push(TokVec *tv, Tok t)
{
    if (tv->n == tv->cap) {
        tv->cap = tv->cap ? tv->cap * 2 : 32;
        tv->v = xrealloc(tv->v, (size_t)tv->cap * sizeof(Tok));
    }
    tv->v[tv->n++] = t;
}

static void tv_free(TokVec *tv)
{
    for (int i = 0; i < tv->n; i++)
        free(tv->v[i].text);
    free(tv->v);
}

static int is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static int is_op_char(char c)
{
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' || c == '(' || c == ')';
}

static int count_nl(const char *s, size_t a, size_t b)
{
    int k = 0;
    for (size_t i = a; i < b; i++)
        k += s[i] == '\n';
    return k;
}

static int lex(const char *s, TokVec *tv)
{
    size_t n = strlen(s), i = 0;
    int line = 1;
    for (;;) {
        /* blanks and backslash-newline continuations between words */
        while (i < n) {
            if (is_blank(s[i])) {
                i++;
            } else if (s[i] == '\\' && i + 1 < n && s[i + 1] == '\n') {
                i += 2;
                line++;
            } else {
                break;
            }
        }
        if (i >= n) {
            Tok t = { T_EOF, xstrdup(""), line, i, i, 0, 0 };
            tv_push(tv, t);
            return PARSE_OK;
        }
        if (s[i] == '\\' && i + 1 >= n)
            return PARSE_INCOMPLETE; /* trailing backslash: continuation */
        char c = s[i];
        if (c == '#') {
            while (i < n && s[i] != '\n')
                i++;
            continue;
        }
        Tok t = { T_EOF, NULL, line, i, i, 0, 0 };
        if (c == '\n') {
            t.type = T_NL;
            t.text = xstrdup("newline");
            t.end = ++i;
            tv_push(tv, t);
            line++;
            continue;
        }

        /* io-number: digits immediately followed by '<' or '>' */
        size_t d = i;
        while (d < n && isdigit((unsigned char)s[d]))
            d++;
        int iofd = -1;
        if (d > i && d < n && (s[d] == '<' || s[d] == '>')) {
            iofd = atoi(s + i);
            i = d;
            c = s[i];
        }

        if (is_op_char(c)) {
            char nx = i + 1 < n ? s[i + 1] : '\0';
            const char *op = NULL;
            size_t oplen = 1;
            t.type = T_REDIR;
            if (c == '<') {
                t.rtype = R_IN;
                t.rfd = 0;
                if (nx == '&') {
                    t.rtype = R_DUP;
                    oplen = 2;
                }
            } else if (c == '>') {
                t.rtype = R_OUT;
                t.rfd = 1;
                if (nx == '>') {
                    t.rtype = R_APPEND;
                    oplen = 2;
                } else if (nx == '&') {
                    t.rtype = R_DUP;
                    oplen = 2;
                } else if (nx == '|') {
                    oplen = 2;
                }
            } else if (iofd >= 0) {
                /* digits followed by a non-redirection operator */
                iofd = -1;
            }
            if (c == '<' || c == '>') {
                if (iofd >= 0)
                    t.rfd = iofd;
            } else if (c == '&' && nx == '>' && iofd < 0) {
                t.rtype = R_BOTH;
                t.rfd = 1;
                oplen = 2;
            } else if (c == '|') {
                t.type = nx == '|' ? T_OROR : T_PIPE;
                oplen = nx == '|' ? 2 : 1;
                op = nx == '|' ? "||" : "|";
            } else if (c == '&') {
                t.type = nx == '&' ? T_ANDAND : T_AMP;
                oplen = nx == '&' ? 2 : 1;
                op = nx == '&' ? "&&" : "&";
            } else if (c == ';') {
                t.type = nx == ';' ? T_DSEMI : T_SEMI;
                oplen = nx == ';' ? 2 : 1;
                op = nx == ';' ? ";;" : ";";
            } else if (c == '(') {
                t.type = T_LPAREN;
                op = "(";
            } else if (c == ')') {
                t.type = T_RPAREN;
                op = ")";
            }
            t.text = op ? xstrdup(op) : xstrndup(s + i, oplen);
            i += oplen;
            t.end = i;
            tv_push(tv, t);
            continue;
        }

        /* a word: keep its raw text (quotes and expansions included) */
        Buf w;
        buf_init(&w);
        size_t start = t.start;
        while (i < n) {
            c = s[i];
            if (is_blank(c) || c == '\n' || is_op_char(c))
                break;
            long e;
            if (c == '\\') {
                if (i + 1 >= n) {
                    buf_free(&w);
                    return PARSE_INCOMPLETE;
                }
                if (s[i + 1] == '\n') { /* line continuation inside a word */
                    i += 2;
                    line++;
                    continue;
                }
                buf_putn(&w, s + i, 2);
                i += 2;
                continue;
            }
            if (c == '\'')
                e = scan_squote(s, i, n);
            else if (c == '"')
                e = scan_dquote(s, i, n);
            else if (c == '`')
                e = scan_backtick(s, i, n);
            else if (c == '$' && i + 1 < n && s[i + 1] == '(')
                e = scan_subst(s, i, n);
            else if (c == '$' && i + 1 < n && s[i + 1] == '{')
                e = scan_brace(s, i, n);
            else {
                buf_putc(&w, c);
                i++;
                continue;
            }
            if (e < 0) {
                buf_free(&w);
                return PARSE_INCOMPLETE;
            }
            buf_putn(&w, s + i, (size_t)e - i);
            line += count_nl(s, i, (size_t)e);
            i = (size_t)e;
        }
        t.type = T_WORD;
        t.text = buf_detach(&w);
        t.start = start;
        t.end = i;
        tv_push(tv, t);
    }
}

/* ================================================================= parser */

typedef struct {
    Tok *t;
    int n, pos;
    const char *src;
    int status;
    char *err;
    size_t errlen;
    int errline;
    size_t last_end;
} Parser;

static Tok *peek(Parser *p)
{
    return &p->t[p->pos];
}

static Tok *advance(Parser *p)
{
    Tok *t = &p->t[p->pos];
    p->last_end = t->end;
    if (p->pos < p->n - 1)
        p->pos++;
    return t;
}

static int is_kw(Tok *t, const char *kw)
{
    return t->type == T_WORD && strcmp(t->text, kw) == 0;
}

static int is_terminator_kw(Tok *t)
{
    return is_kw(t, "then") || is_kw(t, "elif") || is_kw(t, "else") || is_kw(t, "fi");
}

static void fail(Parser *p, Tok *t)
{
    if (p->status != PARSE_OK)
        return;
    if (t->type == T_EOF) {
        p->status = PARSE_INCOMPLETE;
        snprintf(p->err, p->errlen, "syntax error: unexpected end of file");
    } else {
        p->status = PARSE_ERROR;
        snprintf(p->err, p->errlen, "syntax error near unexpected token `%s'", t->text);
    }
    p->errline = t->line;
}

static void skip_newlines(Parser *p)
{
    while (peek(p)->type == T_NL)
        advance(p);
}

static Node *new_node(NodeType type, Parser *p)
{
    Node *n = xcalloc(1, sizeof *n);
    n->type = type;
    n->line = peek(p)->line;
    return n;
}

static void set_text(Node *n, Parser *p, size_t start)
{
    size_t end = p->last_end;
    while (end > start && (p->src[end - 1] == ' ' || p->src[end - 1] == '\t'))
        end--;
    n->text = xstrndup(p->src + start, end > start ? end - start : 0);
}

#define PUSH(arr, cnt, val)                                                    \
    do {                                                                       \
        (arr) = xrealloc((arr), (size_t)((cnt) + 1) * sizeof(*(arr)));         \
        (arr)[(cnt)++] = (val);                                                \
    } while (0)

static Node *parse_list(Parser *p, int stop_kw);
static Node *parse_andor(Parser *p);

static Node *parse_simple(Parser *p)
{
    Node *n = new_node(N_SIMPLE, p);
    size_t start = peek(p)->start;
    Redir **tail = &n->redirs;
    for (;;) {
        Tok *t = peek(p);
        if (t->type == T_WORD) {
            if (n->nwords == 0 && assignment_name_len(t->text))
                PUSH(n->assigns, n->nassigns, xstrdup(t->text));
            else
                PUSH(n->words, n->nwords, xstrdup(t->text));
            advance(p);
        } else if (t->type == T_REDIR) {
            advance(p);
            Tok *target = peek(p);
            if (target->type != T_WORD) {
                if (target->type == T_EOF) {
                    p->status = PARSE_ERROR;
                    snprintf(p->err, p->errlen, "syntax error near unexpected token `newline'");
                    p->errline = target->line;
                } else {
                    fail(p, target);
                }
                break;
            }
            Redir *r = xcalloc(1, sizeof *r);
            r->type = t->rtype;
            r->fd = t->rfd;
            r->target = xstrdup(target->text);
            *tail = r;
            tail = &r->next;
            advance(p);
        } else {
            break;
        }
    }
    if (p->status == PARSE_OK && n->nwords == 0 && n->nassigns == 0 && !n->redirs)
        fail(p, peek(p));
    set_text(n, p, start);
    return n;
}

static Node *parse_if(Parser *p)
{
    Node *n = new_node(N_IF, p);
    size_t start = peek(p)->start;
    advance(p); /* 'if' */
    for (;;) {
        Node *cond = parse_list(p, 1);
        PUSH(n->conds, n->nconds, cond);
        n->bodies = xrealloc(n->bodies, (size_t)n->nconds * sizeof(Node *));
        n->bodies[n->nconds - 1] = NULL;
        if (p->status != PARSE_OK)
            return n;
        if (cond->nitems == 0 || !is_kw(peek(p), "then")) {
            fail(p, peek(p));
            return n;
        }
        advance(p); /* 'then' */
        Node *body = parse_list(p, 1);
        n->bodies[n->nconds - 1] = body;
        if (p->status != PARSE_OK)
            return n;
        if (body->nitems == 0) {
            fail(p, peek(p));
            return n;
        }
        Tok *t = peek(p);
        if (is_kw(t, "elif")) {
            advance(p);
            continue;
        }
        if (is_kw(t, "else")) {
            advance(p);
            n->else_body = parse_list(p, 1);
            if (p->status != PARSE_OK)
                return n;
            if (n->else_body->nitems == 0 || !is_kw(peek(p), "fi")) {
                fail(p, peek(p));
                return n;
            }
            advance(p);
            break;
        }
        if (is_kw(t, "fi")) {
            advance(p);
            break;
        }
        fail(p, t);
        return n;
    }
    set_text(n, p, start);
    return n;
}

static Node *parse_command(Parser *p)
{
    Tok *t = peek(p);
    if (t->type == T_WORD) {
        if (is_kw(t, "if"))
            return parse_if(p);
        if (is_terminator_kw(t)) {
            fail(p, t);
            return NULL;
        }
        return parse_simple(p);
    }
    if (t->type == T_REDIR)
        return parse_simple(p);
    fail(p, t);
    return NULL;
}

static Node *parse_pipeline(Parser *p)
{
    size_t start = peek(p)->start;
    int negate = 0;
    if (is_kw(peek(p), "!")) {
        negate = 1;
        advance(p);
    }
    Node *first = parse_command(p);
    if (!first || p->status != PARSE_OK)
        return first;
    if (peek(p)->type != T_PIPE && !negate)
        return first;
    Node *n = new_node(N_PIPE, p);
    n->negate = negate;
    PUSH(n->cmds, n->ncmds, first);
    while (peek(p)->type == T_PIPE) {
        advance(p);
        skip_newlines(p);
        Node *c = parse_command(p);
        if (!c)
            break;
        PUSH(n->cmds, n->ncmds, c);
        if (p->status != PARSE_OK)
            break;
    }
    set_text(n, p, start);
    return n;
}

static Node *parse_andor(Parser *p)
{
    size_t start = peek(p)->start;
    Node *first = parse_pipeline(p);
    if (!first || p->status != PARSE_OK)
        return first;
    Tok *t = peek(p);
    if (t->type != T_ANDAND && t->type != T_OROR)
        return first;
    Node *n = new_node(N_ANDOR, p);
    PUSH(n->parts, n->nparts, first);
    int nops = 0;
    while ((t = peek(p))->type == T_ANDAND || t->type == T_OROR) {
        int op = t->type == T_ANDAND ? 1 : 2;
        advance(p);
        skip_newlines(p);
        Node *c = parse_pipeline(p);
        if (!c)
            break;
        PUSH(n->ops, nops, op);
        PUSH(n->parts, n->nparts, c);
        if (p->status != PARSE_OK)
            break;
    }
    set_text(n, p, start);
    return n;
}

static Node *parse_list(Parser *p, int stop_kw)
{
    Node *list = new_node(N_LIST, p);
    size_t start = peek(p)->start;
    skip_newlines(p);
    for (;;) {
        Tok *t = peek(p);
        if (t->type == T_EOF)
            break;
        if (stop_kw && is_terminator_kw(t))
            break;
        Node *item = parse_andor(p);
        if (item) {
            PUSH(list->items, list->nitems, item);
            list->bg = xrealloc(list->bg, (size_t)list->nitems * sizeof(int));
            list->bg[list->nitems - 1] = 0;
        }
        if (p->status != PARSE_OK)
            break;
        t = peek(p);
        if (t->type == T_AMP) {
            list->bg[list->nitems - 1] = 1;
            advance(p);
        } else if (t->type == T_SEMI || t->type == T_NL) {
            advance(p);
        } else if (t->type == T_EOF || (stop_kw && is_terminator_kw(t))) {
            break;
        } else {
            fail(p, t);
            break;
        }
        skip_newlines(p);
    }
    set_text(list, p, start);
    return list;
}

int parse_program(const char *src, Node **out, char *err, size_t errlen, int *errline)
{
    TokVec tv = { 0 };
    *out = NULL;
    if (errline)
        *errline = 0;
    int r = lex(src, &tv);
    if (r != PARSE_OK) {
        snprintf(err, errlen, "syntax error: unexpected end of file");
        if (errline) {
            int lines = 1;
            for (const char *c = src; *c; c++)
                lines += *c == '\n';
            *errline = lines;
        }
        tv_free(&tv);
        return r;
    }
    Parser p = { tv.v, tv.n, 0, src, PARSE_OK, err, errlen, 0, 0 };
    Node *root = parse_list(&p, 0);
    if (p.status == PARSE_OK && peek(&p)->type != T_EOF)
        fail(&p, peek(&p));
    tv_free(&tv);
    if (p.status != PARSE_OK) {
        if (errline)
            *errline = p.errline;
        node_free(root);
        return p.status;
    }
    *out = root;
    return PARSE_OK;
}

void node_free(Node *n)
{
    if (!n)
        return;
    free(n->text);
    for (int i = 0; i < n->nitems; i++)
        node_free(n->items[i]);
    free(n->items);
    free(n->bg);
    for (int i = 0; i < n->nparts; i++)
        node_free(n->parts[i]);
    free(n->parts);
    free(n->ops);
    for (int i = 0; i < n->ncmds; i++)
        node_free(n->cmds[i]);
    free(n->cmds);
    for (int i = 0; i < n->nassigns; i++)
        free(n->assigns[i]);
    free(n->assigns);
    for (int i = 0; i < n->nwords; i++)
        free(n->words[i]);
    free(n->words);
    for (Redir *r = n->redirs, *nx; r; r = nx) {
        nx = r->next;
        free(r->target);
        free(r);
    }
    for (int i = 0; i < n->nconds; i++) {
        node_free(n->conds[i]);
        node_free(n->bodies[i]);
    }
    free(n->conds);
    free(n->bodies);
    node_free(n->else_body);
    free(n);
}
