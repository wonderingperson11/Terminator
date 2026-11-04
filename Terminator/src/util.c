/*
 * util.c - see util.h.
 */
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

static void oom(void)
{
    static const char msg[] = "terminator: out of memory\n";
    (void)!write(2, msg, sizeof msg - 1);
    _exit(1);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        oom();
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p)
        oom();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        oom();
    return p;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *xasprintf(const char *fmt, ...)
{
    Buf b;
    va_list ap;
    buf_init(&b);
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    buf_reserve(&b, (size_t)need + 1);
    va_start(ap, fmt);
    vsnprintf(b.s, (size_t)need + 1, fmt, ap);
    va_end(ap);
    b.len = (size_t)need;
    return buf_detach(&b);
}

/* ---------------------------------------------------------------- Buf */

void buf_init(Buf *b)
{
    b->s = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf *b)
{
    free(b->s);
    buf_init(b);
}

void buf_clear(Buf *b)
{
    b->len = 0;
    if (b->s)
        b->s[0] = '\0';
}

void buf_reserve(Buf *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 32;
    while (cap < need)
        cap *= 2;
    b->s = xrealloc(b->s, cap);
    b->cap = cap;
}

void buf_putc(Buf *b, char c)
{
    buf_reserve(b, 1);
    b->s[b->len++] = c;
    b->s[b->len] = '\0';
}

void buf_putn(Buf *b, const char *s, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

void buf_puts(Buf *b, const char *s)
{
    buf_putn(b, s, strlen(s));
}

void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need <= 0)
        return;
    buf_reserve(b, (size_t)need);
    va_start(ap, fmt);
    vsnprintf(b->s + b->len, (size_t)need + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)need;
}

void buf_insert(Buf *b, size_t pos, const char *s, size_t n)
{
    if (pos > b->len)
        pos = b->len;
    buf_reserve(b, n);
    memmove(b->s + pos + n, b->s + pos, b->len - pos + 1);
    memcpy(b->s + pos, s, n);
    b->len += n;
}

void buf_erase(Buf *b, size_t pos, size_t n)
{
    if (pos >= b->len)
        return;
    if (pos + n > b->len)
        n = b->len - pos;
    memmove(b->s + pos, b->s + pos + n, b->len - pos - n + 1);
    b->len -= n;
}

char *buf_detach(Buf *b)
{
    char *s = b->s ? b->s : xstrdup("");
    buf_init(b);
    return s;
}

/* ------------------------------------------------------------- StrVec */

void sv_init(StrVec *sv)
{
    sv->v = NULL;
    sv->n = 0;
    sv->cap = 0;
}

void sv_push(StrVec *sv, char *s)
{
    if (sv->n + 2 > sv->cap) {
        sv->cap = sv->cap ? sv->cap * 2 : 8;
        sv->v = xrealloc(sv->v, (size_t)sv->cap * sizeof(char *));
    }
    sv->v[sv->n++] = s;
    sv->v[sv->n] = NULL;
}

void sv_pushdup(StrVec *sv, const char *s)
{
    sv_push(sv, xstrdup(s));
}

void sv_free(StrVec *sv)
{
    for (int i = 0; i < sv->n; i++)
        free(sv->v[i]);
    free(sv->v);
    sv_init(sv);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void sv_sort(StrVec *sv)
{
    if (sv->n > 1)
        qsort(sv->v, (size_t)sv->n, sizeof(char *), cmp_str);
}

/* ---------------------------------------------------------------- I/O */

ssize_t read_retry(int fd, void *buf, size_t n)
{
    ssize_t r;
    do {
        r = read(fd, buf, n);
    } while (r < 0 && errno == EINTR);
    return r;
}

int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                /* Non-blocking peer is full: wait until writable. */
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(fd, &wfds);
                select(fd + 1, NULL, &wfds, NULL, NULL);
                continue;
            }
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

void set_cloexec(int fd, int on)
{
    int fl = fcntl(fd, F_GETFD);
    if (fl < 0)
        return;
    fcntl(fd, F_SETFD, on ? (fl | FD_CLOEXEC) : (fl & ~FD_CLOEXEC));
}

void set_nonblock(int fd, int on)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0)
        return;
    fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

int make_pipe(int fds[2], int cloexec)
{
#ifdef __linux__
    if (pipe2(fds, cloexec ? O_CLOEXEC : 0) < 0)
        return -1;
#else
    if (pipe(fds) < 0)
        return -1;
    if (cloexec) {
        set_cloexec(fds[0], 1);
        set_cloexec(fds[1], 1);
    }
#endif
    return 0;
}

void close_from(int lowfd)
{
#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, (unsigned)lowfd, ~0U, 0) == 0)
        return;
#endif
    /* Collect the open descriptors first: closing while iterating would
     * close the directory stream's own descriptor. */
    int fds[1024];
    int nfds = 0;
#ifdef __linux__
    DIR *d = opendir("/proc/self/fd");
#else
    DIR *d = opendir("/dev/fd");
#endif
    if (d) {
        int self = dirfd(d);
        struct dirent *de;
        while ((de = readdir(d)) && nfds < (int)ARRAY_LEN(fds)) {
            int fd = atoi(de->d_name);
            if (de->d_name[0] != '.' && fd >= lowfd && fd != self)
                fds[nfds++] = fd;
        }
        closedir(d);
        for (int i = 0; i < nfds; i++)
            close(fds[i]);
        return;
    }
    long max = sysconf(_SC_OPEN_MAX);
    if (max < 0 || max > 65536)
        max = 65536;
    for (int fd = lowfd; fd < max; fd++)
        close(fd);
}

char *read_file(const char *path, size_t *len_out)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    Buf b;
    buf_init(&b);
    char tmp[8192];
    ssize_t r;
    while ((r = read_retry(fd, tmp, sizeof tmp)) > 0)
        buf_putn(&b, tmp, (size_t)r);
    close(fd);
    if (r < 0) {
        buf_free(&b);
        return NULL;
    }
    if (len_out)
        *len_out = b.len;
    return buf_detach(&b);
}

long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

const char *home_dir(void)
{
    static char cached[4096];
    const char *h = getenv("HOME");
    if (h && *h)
        return h;
    if (!cached[0]) {
        struct passwd pw, *res = NULL;
        char tmp[4096];
        if (getpwuid_r(getuid(), &pw, tmp, sizeof tmp, &res) == 0 && res)
            snprintf(cached, sizeof cached, "%s", pw.pw_dir);
        else
            snprintf(cached, sizeof cached, "/");
    }
    return cached;
}

char *expand_home(const char *path)
{
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0'))
        return xasprintf("%s%s", home_dir(), path + 1);
    return xstrdup(path);
}

char *collapse_home(const char *path)
{
    const char *h = home_dir();
    size_t hl = strlen(h);
    if (hl > 1 && strncmp(path, h, hl) == 0 && (path[hl] == '/' || path[hl] == '\0'))
        return xasprintf("~%s", path + hl);
    return xstrdup(path);
}

/* ---------------------------------------------------------------- UTF-8 */

int utf8_decode(const unsigned char *s, size_t n, unsigned *cp)
{
    if (n == 0)
        return 0;
    unsigned c = s[0];
    int len;
    if (c < 0x80) {
        *cp = c;
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
        len = 2;
        c &= 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        c &= 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        c &= 0x07;
    } else {
        *cp = 0xFFFD;
        return 1;
    }
    if ((size_t)len > n)
        return -1; /* incomplete sequence */
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *cp = c;
    return len;
}

void latin1_to_utf8(Buf *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            buf_putc(out, (char)c);
        } else {
            buf_putc(out, (char)(0xC0 | (c >> 6)));
            buf_putc(out, (char)(0x80 | (c & 0x3F)));
        }
    }
}
