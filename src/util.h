/*
 * util.h - small helpers shared by every part of Terminator:
 * checked allocation, growable strings/vectors, safe I/O wrappers.
 */
#ifndef TERMINATOR_UTIL_H
#define TERMINATOR_UTIL_H

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Growable, always NUL-terminated byte buffer. */
typedef struct {
    char *s;
    size_t len;
    size_t cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_clear(Buf *b);
void buf_reserve(Buf *b, size_t extra);
void buf_putc(Buf *b, char c);
void buf_putn(Buf *b, const char *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_insert(Buf *b, size_t pos, const char *s, size_t n);
void buf_erase(Buf *b, size_t pos, size_t n);
char *buf_detach(Buf *b); /* returns heap string, leaves b empty */

/* Growable vector of heap strings, NULL-terminated (usable as argv). */
typedef struct {
    char **v;
    int n;
    int cap;
} StrVec;

void sv_init(StrVec *sv);
void sv_push(StrVec *sv, char *s); /* takes ownership */
void sv_pushdup(StrVec *sv, const char *s);
void sv_free(StrVec *sv);
void sv_sort(StrVec *sv);

/* I/O helpers that retry on EINTR. */
ssize_t read_retry(int fd, void *buf, size_t n);
int write_all(int fd, const void *buf, size_t n); /* 0 ok, -1 error */
int make_pipe(int fds[2], int cloexec);
void set_cloexec(int fd, int on);
void set_nonblock(int fd, int on);
void close_from(int lowfd); /* close every fd >= lowfd */
char *read_file(const char *path, size_t *len_out);

long long now_ms(void);
const char *home_dir(void);
char *expand_home(const char *path); /* "~/x" -> "/home/u/x" (heap) */
char *collapse_home(const char *path); /* "/home/u/x" -> "~/x" (heap) */

/* UTF-8 helpers (the shell side is UTF-8; the X11 font is Latin-1). */
int utf8_decode(const unsigned char *s, size_t n, unsigned *cp); /* bytes used */
void latin1_to_utf8(Buf *out, const char *s, size_t n);

#endif
