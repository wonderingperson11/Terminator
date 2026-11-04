/*
 * parse.h - lexer and recursive-descent parser for the Terminator shell
 * language (a practical subset of bash).
 *
 * Words are kept in their raw form (quotes and $-expressions intact); they
 * are expanded at execution time, so variables, $? and command substitution
 * always see the current state of the shell.
 */
#ifndef TERMINATOR_PARSE_H
#define TERMINATOR_PARSE_H

#include <stddef.h>

enum { PARSE_OK = 0, PARSE_INCOMPLETE = 1, PARSE_ERROR = 2 };

typedef enum {
    R_IN,      /* n<  file  */
    R_OUT,     /* n>  file  */
    R_APPEND,  /* n>> file  */
    R_DUP,     /* n>&m / n<&m */
    R_BOTH,    /* &> file   */
} RedirType;

typedef struct Redir {
    RedirType type;
    int fd;          /* descriptor being redirected */
    char *target;    /* raw word (file name or fd number for R_DUP) */
    struct Redir *next;
} Redir;

typedef enum { N_LIST, N_ANDOR, N_PIPE, N_SIMPLE, N_IF } NodeType;

typedef struct Node Node;
struct Node {
    NodeType type;
    char *text;      /* source text of this node (for job listings) */
    int line;        /* line number in the source */

    /* N_LIST: items run in order; bg[i] => item i ends with '&' */
    Node **items;
    int *bg;
    int nitems;

    /* N_ANDOR: parts[0] op[0] parts[1] op[1] ... ; op is 1 for &&, 2 for || */
    Node **parts;
    int *ops;
    int nparts;

    /* N_PIPE */
    Node **cmds;
    int ncmds;
    int negate;

    /* N_SIMPLE */
    char **assigns;  /* raw NAME=value words */
    int nassigns;
    char **words;    /* raw words */
    int nwords;
    Redir *redirs;

    /* N_IF: if conds[0] then bodies[0] elif conds[1] then bodies[1] ... */
    Node **conds;
    Node **bodies;
    int nconds;
    Node *else_body;
};

/*
 * Parse a complete program.  On PARSE_OK *out is the root (an N_LIST, maybe
 * empty).  PARSE_INCOMPLETE means the input ended in the middle of a
 * construct (open quote, trailing '\' or '|', 'if' without 'fi', ...): an
 * interactive caller asks for another line.  On PARSE_ERROR, err holds a
 * bash-style message such as "syntax error near unexpected token `fi'".
 */
int parse_program(const char *src, Node **out, char *err, size_t errlen, int *errline);
void node_free(Node *n);

/* Scanner helpers shared with the expander.  Given s[i] == '$' and
 * s[i+1] == '(' (or s[i] == '`'), return the index just past the matching
 * ')' (or '`'), or -1 if the input ends first. */
long scan_subst(const char *s, size_t i, size_t n);
long scan_backtick(const char *s, size_t i, size_t n);
long scan_brace(const char *s, size_t i, size_t n); /* ${...} */

int is_name_start(int c);
int is_name_char(int c);
/* Length of the NAME in a "NAME=value" word, or 0 if it isn't one. */
size_t assignment_name_len(const char *w);

#endif
