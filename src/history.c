/*
 * history.c - command history shared by every tab.
 *
 * The history lives in ONE place: a shared-memory segment (shm_open +
 * mmap(MAP_SHARED)) created by the GUI before any tab exists, so tab
 * threads (thread mode) and forked tab processes (process mode) all see the
 * same ring buffer.  It is protected by a process-shared POSIX semaphore
 * stored inside the segment (sem_init(&sem, pshared = 1, 1)).
 *
 * The last HIST_MAX commands are also kept in ~/.terminator_history (one
 * per line; a newline inside a multi-line command is stored as \x1f).
 */
#include "shell.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HIST_MAX 10000
#define HIST_SHOW 1000
#define HIST_LEN 2048
#define NL_MARK '\x1f'

typedef struct {
#ifndef __APPLE__
    sem_t lock;            /* process-shared */
#endif
    int count;             /* entries in use (<= HIST_MAX) */
    int head;              /* index of the oldest entry */
    int file_lines;        /* lines currently in the history file */
    unsigned long total;   /* commands ever added (for numbering) */
    char path[1024];
    char e[HIST_MAX][HIST_LEN];
} HistShm;

static HistShm *H;
#ifdef __APPLE__
/* macOS has no unnamed process-shared semaphores; a named one (unlinked
 * right away) behaves the same across fork().  Only used for local builds. */
static sem_t *apple_sem;
#define LOCKP apple_sem
#else
#define LOCKP (&H->lock)
#endif

void hist_lock(void)
{
    if (!H)
        return;
#ifdef __APPLE__
    while (sem_wait(LOCKP) < 0 && errno == EINTR)
        ;
#else
    /* A tab process that crashes while holding the lock must not freeze the
     * other tabs: give up waiting after a while and carry on. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;
    while (sem_timedwait(LOCKP, &ts) < 0) {
        if (errno == EINTR)
            continue;
        if (errno == ETIMEDOUT)
            fprintf(stderr, "terminator: history lock timed out (holder died?)\n");
        break;
    }
#endif
}

void hist_unlock(void)
{
    if (H)
        sem_post(LOCKP);
}

/* fork() while another thread holds the lock would leave the child with a
 * locked copy of the state it protects: take the lock around fork().  The
 * semaphore is shared memory, so only the parent releases it. */
static void atfork_prepare(void)
{
    hist_lock();
}

static void atfork_parent(void)
{
    hist_unlock();
}

static void atfork_child(void)
{
}

static void store(const char *cmd)
{
    int idx;
    if (H->count < HIST_MAX) {
        idx = (H->head + H->count) % HIST_MAX;
        H->count++;
    } else {
        idx = H->head;
        H->head = (H->head + 1) % HIST_MAX;
    }
    snprintf(H->e[idx], HIST_LEN, "%s", cmd);
    H->total++;
}

static const char *entry(int i) /* 0 = oldest */
{
    return H->e[(H->head + i) % HIST_MAX];
}

static void encode_line(Buf *b, const char *s)
{
    for (; *s; s++)
        buf_putc(b, *s == '\n' ? NL_MARK : *s);
    buf_putc(b, '\n');
}

static void rewrite_file(void)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp%ld", H->path, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    Buf b;
    buf_init(&b);
    for (int i = 0; i < H->count; i++)
        encode_line(&b, entry(i));
    int ok = write_all(fd, b.s ? b.s : "", b.len) == 0;
    close(fd);
    buf_free(&b);
    if (ok && rename(tmp, H->path) == 0)
        H->file_lines = H->count;
    else
        unlink(tmp);
}

static void load_file(void)
{
    size_t len;
    char *data = read_file(H->path, &len);
    if (!data)
        return;
    int lines = 0;
    char *p = data;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';
        for (char *c = p; *c; c++)
            if (*c == NL_MARK)
                *c = '\n';
        if (*p)
            store(p);
        lines++;
        if (!nl)
            break;
        p = nl + 1;
    }
    H->file_lines = lines;
    free(data);
    if (lines > HIST_MAX)
        rewrite_file(); /* keep only the last HIST_MAX commands */
}

int hist_init(void)
{
    size_t size = sizeof(HistShm);
    char name[64];
    snprintf(name, sizeof name, "/terminator-hist-%ld", (long)getpid());
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    void *mem = MAP_FAILED;
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)size) == 0)
            mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        shm_unlink(name); /* the mapping stays; children inherit it */
    }
    if (mem == MAP_FAILED)
        mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED)
        return -1;
    H = mem;
    H->count = H->head = H->file_lines = 0;
    H->total = 0;
#ifdef __APPLE__
    snprintf(name, sizeof name, "/term-hl-%ld", (long)getpid());
    apple_sem = sem_open(name, O_CREAT | O_EXCL, 0600, 1);
    sem_unlink(name);
#else
    if (sem_init(&H->lock, 1, 1) < 0)
        return -1;
#endif
    const char *hf = getenv("HISTFILE");
    if (hf && *hf)
        snprintf(H->path, sizeof H->path, "%s", hf);
    else
        snprintf(H->path, sizeof H->path, "%s/.terminator_history", home_dir());
    load_file();
    pthread_atfork(atfork_prepare, atfork_parent, atfork_child);
    return 0;
}

void hist_add(const char *cmd)
{
    if (!H || !cmd || !*cmd)
        return;
    hist_lock();
    store(cmd);
    Buf b;
    buf_init(&b);
    encode_line(&b, cmd);
    int fd = open(H->path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) {
        write_all(fd, b.s, b.len);
        close(fd);
        H->file_lines++;
    }
    buf_free(&b);
    if (H->file_lines > HIST_MAX + HIST_MAX / 10)
        rewrite_file();
    hist_unlock();
}

int hist_count(void)
{
    if (!H)
        return 0;
    hist_lock();
    int n = H->count;
    hist_unlock();
    return n;
}

char *hist_get_recent(int back)
{
    if (!H || back < 0)
        return NULL;
    char *r = NULL;
    hist_lock();
    if (back < H->count)
        r = xstrdup(entry(H->count - 1 - back));
    hist_unlock();
    return r;
}

void hist_clear(void)
{
    if (!H)
        return;
    hist_lock();
    H->count = 0;
    H->head = 0;
    rewrite_file();
    hist_unlock();
}

void hist_show(Shell *sh, int max)
{
    if (!H)
        return;
    if (max <= 0 || max > HIST_SHOW)
        max = HIST_SHOW;
    Buf b;
    buf_init(&b);
    hist_lock();
    int n = H->count < max ? H->count : max;
    unsigned long first = H->total - (unsigned long)H->count + 1;
    for (int i = H->count - n; i < H->count; i++)
        buf_printf(&b, "%5lu  %s\n", first + (unsigned long)i, entry(i));
    hist_unlock();
    sh_write(sh, sh->fd_out, b.s ? b.s : "", b.len);
    buf_free(&b);
}

/* length of the longest common substring of a and b */
static size_t lcs_len(const char *a, const char *b, size_t *row_prev, size_t *row_cur)
{
    size_t n = strlen(a), m = strlen(b), best = 0;
    for (size_t j = 0; j <= m; j++)
        row_prev[j] = 0;
    for (size_t i = 1; i <= n; i++) {
        row_cur[0] = 0;
        for (size_t j = 1; j <= m; j++) {
            row_cur[j] = a[i - 1] == b[j - 1] ? row_prev[j - 1] + 1 : 0;
            if (row_cur[j] > best)
                best = row_cur[j];
        }
        size_t *t = row_prev;
        row_prev = row_cur;
        row_cur = t;
    }
    return best;
}

/* Ctrl+R: (a) the most recent exact match, else (b) every command sharing
 * the longest common substring with the term if that is longer than two
 * characters, else (c) "No match for search term in history". */
void hist_search(Shell *sh, const char *term)
{
    StrVec res;
    sv_init(&res);
    size_t m = strlen(term);
    size_t *r1 = xcalloc(m + 1, sizeof(size_t));
    size_t *r2 = xcalloc(m + 1, sizeof(size_t));
    size_t best = 0;

    hist_lock();
    int count = H ? H->count : 0;
    for (int i = count - 1; i >= 0; i--) {
        if (strcmp(entry(i), term) == 0) {
            sv_pushdup(&res, entry(i));
            break;
        }
    }
    if (res.n == 0) {
        size_t *score = xcalloc((size_t)count + 1, sizeof(size_t));
        for (int i = 0; i < count; i++) {
            score[i] = lcs_len(entry(i), term, r1, r2);
            if (score[i] > best)
                best = score[i];
        }
        if (best > 2) {
            for (int i = count - 1; i >= 0; i--) {
                if (score[i] != best)
                    continue;
                int dup = 0;
                for (int k = 0; k < res.n && !dup; k++)
                    dup = strcmp(res.v[k], entry(i)) == 0;
                if (!dup)
                    sv_pushdup(&res, entry(i));
            }
        }
        free(score);
    }
    hist_unlock();

    if (res.n == 0) {
        sh_out(sh, "No match for search term in history\n");
    } else {
        for (int i = 0; i < res.n; i++) {
            sh_out(sh, res.v[i]);
            sh_out(sh, "\n");
        }
    }
    sv_free(&res);
    free(r1);
    free(r2);
}
