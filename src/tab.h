/*
 * tab.h - starting a tab's shell and talking to it.
 *
 *   thread mode : pthread_create() a shell thread in this process
 *   process mode: fork() a shell process
 *
 * In both modes the GUI side sees the same thing: a pair of pipes carrying
 * proto.h messages.  That is what makes the two modes behave identically.
 */
#ifndef TERMINATOR_TAB_H
#define TERMINATOR_TAB_H

#include "proto.h"
#include "util.h"

#include <pthread.h>
#include <sys/types.h>

typedef struct {
    int to_shell;     /* GUI -> shell (non-blocking, queued) */
    int from_shell;   /* shell -> GUI (non-blocking)         */
    int process_mode;
    pid_t pid;        /* process mode */
    pthread_t thread; /* thread mode  */
    int has_thread;
    MsgReader rd;
    Buf txq;          /* bytes not yet written to to_shell */
    int eof;          /* the shell side has gone */
    int got_exit;     /* it said goodbye with M_EXIT */
    int exit_status;
    int crashed;      /* process mode: killed by a signal */
    int crash_signal;
} TabConn;

/* fds_to_close: GUI-side descriptors a forked tab process must not keep
 * (the X connection, other tabs' pipes). */
int tabconn_spawn(TabConn *tc, int process_mode, const char *start_dir, const int *fds_to_close,
                  int nclose);
void tabconn_send(TabConn *tc, int type, const void *a, size_t alen, const void *b, size_t blen);
void tabconn_send_key(TabConn *tc, int action, const char *text, size_t len);
void tabconn_send_resize(TabConn *tc, int cols, int rows);
int tabconn_flush(TabConn *tc);     /* 1 if data is still queued */
void tabconn_close_input(TabConn *tc);
void tabconn_finish(TabConn *tc);   /* after EOF: join / waitpid */

#endif
