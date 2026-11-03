/*
 * wake.c - SIGCHLD self-pipe trick, safe for several shells per process.
 *
 * A signal is delivered to an arbitrary thread of the process, so the
 * handler cannot know which tab's child changed state.  Every shell owns a
 * "slot" with a pipe; the handler writes one byte into the pipe of every
 * active slot and each shell's poll() loop wakes up and reaps only its own
 * children with waitpid(pid, WNOHANG).
 *
 * Pipes are created once per slot and never closed, so the handler can
 * never write into a descriptor that was closed and reused for something
 * else.
 */
#include "shell.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#define MAX_WAKE 128

static int wake_rd[MAX_WAKE];
static int wake_wr[MAX_WAKE];
static volatile sig_atomic_t wake_active[MAX_WAKE];
static int wake_made[MAX_WAKE];
static pthread_mutex_t wake_mu = PTHREAD_MUTEX_INITIALIZER;

static void sigchld_handler(int sig)
{
    (void)sig;
    int saved = errno;
    for (int i = 0; i < MAX_WAKE; i++)
        if (wake_active[i])
            (void)!write(wake_wr[i], "c", 1);
    errno = saved;
}

void wake_install_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; /* no SA_NOCLDSTOP: Ctrl+Z stops must wake us */
    sigaction(SIGCHLD, &sa, NULL);
}

int wake_register(int *read_fd)
{
    pthread_mutex_lock(&wake_mu);
    int slot = -1;
    for (int i = 0; i < MAX_WAKE; i++) {
        if (!wake_active[i]) {
            slot = i;
            break;
        }
    }
    if (slot >= 0 && !wake_made[slot]) {
        int p[2];
        if (make_pipe(p, 1) < 0) {
            pthread_mutex_unlock(&wake_mu);
            return -1;
        }
        set_nonblock(p[0], 1);
        set_nonblock(p[1], 1);
        wake_rd[slot] = p[0];
        wake_wr[slot] = p[1];
        wake_made[slot] = 1;
    }
    if (slot >= 0) {
        wake_drain(wake_rd[slot]);
        wake_active[slot] = 1;
        *read_fd = wake_rd[slot];
    }
    pthread_mutex_unlock(&wake_mu);
    return slot;
}

void wake_unregister(int slot)
{
    if (slot < 0 || slot >= MAX_WAKE)
        return;
    pthread_mutex_lock(&wake_mu);
    wake_active[slot] = 0;
    pthread_mutex_unlock(&wake_mu);
}

void wake_drain(int fd)
{
    char tmp[256];
    while (read(fd, tmp, sizeof tmp) > 0)
        ;
}

void wake_reset_child(void)
{
    /* In a freshly forked child: no shell of the parent lives here. */
    for (int i = 0; i < MAX_WAKE; i++)
        wake_active[i] = 0;
    signal(SIGCHLD, SIG_DFL);
}
