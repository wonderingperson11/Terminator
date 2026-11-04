/*
 * jobs.c - job control.
 *
 * Every pipeline started by an interactive shell is a job with its own
 * process group (setpgid).  Keys arrive as X11 events, not as terminal
 * signals, so Ctrl+C / Ctrl+Z are turned into kill(-pgid, SIGINT/SIGTSTP)
 * by the shell itself.  State changes are collected with
 * waitpid(pid, WNOHANG | WUNTRACED | WCONTINUED) for this shell's own pids
 * only, so tabs sharing one process never reap each other's children.
 */
#include "shell.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

Job *job_new(Shell *sh, const char *cmd)
{
    Job *j = xcalloc(1, sizeof *j);
    int maxid = 0;
    Job **tail = &sh->jobs;
    for (Job *k = sh->jobs; k; k = k->next) {
        if (k->id > maxid)
            maxid = k->id;
        tail = &k->next;
    }
    j->id = maxid + 1;
    j->cmd = xstrdup(cmd ? cmd : "");
    j->stdin_w = -1;
    j->state = JOB_RUNNING;
    *tail = j;
    return j;
}

void job_add_pid(Job *j, pid_t pid)
{
    j->pids = xrealloc(j->pids, (size_t)(j->npids + 1) * sizeof(pid_t));
    j->status = xrealloc(j->status, (size_t)(j->npids + 1) * sizeof(int));
    j->finished = xrealloc(j->finished, (size_t)(j->npids + 1) * sizeof(int));
    j->stopped = xrealloc(j->stopped, (size_t)(j->npids + 1) * sizeof(int));
    j->pids[j->npids] = pid;
    j->status[j->npids] = 0;
    j->finished[j->npids] = 0;
    j->stopped[j->npids] = 0;
    j->npids++;
}

void job_free(Job *j)
{
    if (j->stdin_w >= 0)
        close(j->stdin_w);
    free(j->pids);
    free(j->status);
    free(j->finished);
    free(j->stopped);
    free(j->cmd);
    free(j);
}

static int highest_other(Shell *sh, int a, int b)
{
    int best = 0;
    for (Job *k = sh->jobs; k; k = k->next)
        if (k->id != a && k->id != b && k->id > best)
            best = k->id;
    return best;
}

void job_remove(Shell *sh, Job *j)
{
    for (Job **pp = &sh->jobs; *pp; pp = &(*pp)->next) {
        if (*pp == j) {
            *pp = j->next;
            break;
        }
    }
    if (sh->cur_job == j->id) {
        sh->cur_job = sh->prev_job;
        sh->prev_job = highest_other(sh, sh->cur_job, j->id);
    } else if (sh->prev_job == j->id) {
        sh->prev_job = highest_other(sh, sh->cur_job, j->id);
    }
    job_free(j);
}

void job_set_current(Shell *sh, Job *j)
{
    if (sh->cur_job != j->id) {
        sh->prev_job = sh->cur_job;
        sh->cur_job = j->id;
    }
}

static void job_update_state(Job *j)
{
    int running = 0, stopped = 0, alive = 0;
    for (int i = 0; i < j->npids; i++) {
        if (j->finished[i])
            continue;
        alive++;
        if (j->stopped[i])
            stopped++;
        else
            running++;
    }
    if (alive == 0)
        j->state = JOB_DONE;
    else if (running == 0 && stopped > 0)
        j->state = JOB_STOPPED;
    else
        j->state = JOB_RUNNING;
}

void jobs_reap(Shell *sh)
{
    for (Job *j = sh->jobs; j; j = j->next) {
        for (int i = 0; i < j->npids; i++) {
            if (j->finished[i])
                continue;
            int st;
            pid_t r;
            while ((r = waitpid(j->pids[i], &st, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
                if (WIFSTOPPED(st)) {
                    j->stopped[i] = 1;
                } else if (WIFCONTINUED(st)) {
                    j->stopped[i] = 0;
                } else {
                    j->finished[i] = 1;
                    j->status[i] = st;
                    break;
                }
            }
            if (r < 0 && errno == ECHILD)
                j->finished[i] = 1;
        }
        job_update_state(j);
    }
}

int job_last_status(Job *j)
{
    if (j->npids == 0)
        return 0;
    return status_to_code(j->status[j->npids - 1]);
}

static const char *state_text(Job *j, char *tmp, size_t tmplen)
{
    if (j->state == JOB_RUNNING)
        return "Running";
    if (j->state == JOB_STOPPED)
        return "Stopped";
    int st = j->npids ? j->status[j->npids - 1] : 0;
    if (WIFSIGNALED(st)) {
        snprintf(tmp, tmplen, "%s", strsignal(WTERMSIG(st)));
        return tmp;
    }
    if (WIFEXITED(st) && WEXITSTATUS(st) != 0) {
        snprintf(tmp, tmplen, "Exit %d", WEXITSTATUS(st));
        return tmp;
    }
    return "Done";
}

void job_print(Shell *sh, Job *j, int with_state)
{
    char mark = j->id == sh->cur_job ? '+' : j->id == sh->prev_job ? '-' : ' ';
    if (!with_state) {
        sh_printf(sh, "[%d]%c %s%s\n", j->id, mark, j->cmd, j->state == JOB_RUNNING ? " &" : "");
        return;
    }
    char tmp[64];
    sh_printf(sh, "[%d]%c  %-24s%s%s\n", j->id, mark, state_text(j, tmp, sizeof tmp), j->cmd,
              j->state == JOB_RUNNING ? " &" : "");
}

void jobs_notify(Shell *sh)
{
    jobs_reap(sh);
    for (Job *j = sh->jobs, *nx; j; j = nx) {
        nx = j->next;
        if (j->state == JOB_DONE) {
            job_print(sh, j, 1);
            job_remove(sh, j);
        }
    }
}

int jobs_stopped_count(Shell *sh)
{
    int n = 0;
    jobs_reap(sh);
    for (Job *j = sh->jobs; j; j = j->next)
        n += j->state == JOB_STOPPED;
    return n;
}

Job *job_find(Shell *sh, const char *spec, int *err)
{
    *err = 0;
    int id = -1;
    if (!spec || strcmp(spec, "%%") == 0 || strcmp(spec, "%+") == 0 || strcmp(spec, "+") == 0 ||
        strcmp(spec, "%") == 0) {
        id = sh->cur_job;
    } else if (strcmp(spec, "%-") == 0 || strcmp(spec, "-") == 0) {
        id = sh->prev_job;
    } else {
        const char *s = spec[0] == '%' ? spec + 1 : spec;
        char *end;
        long v = strtol(s, &end, 10);
        if (*s && *end == '\0') {
            id = (int)v;
        } else {
            for (Job *j = sh->jobs; j; j = j->next)
                if (strncmp(j->cmd, s, strlen(s)) == 0)
                    return j;
        }
    }
    for (Job *j = sh->jobs; j; j = j->next)
        if (j->id == id)
            return j;
    *err = 1;
    return NULL;
}

/* Wait for a foreground job while staying responsive to the GUI: typed
 * lines go to the job's stdin, Ctrl+C / Ctrl+Z become signals, Ctrl+D
 * closes the job's stdin. */
int job_wait_fg(Shell *sh, Job *j)
{
    j->background = 0;
    ed_begin(sh, ED_JOB, "");
    int closed = 0;
    for (;;) {
        jobs_reap(sh);
        if (j->state != JOB_RUNNING)
            break;
        int ev = shell_poll_input(sh);
        switch (ev) {
        case EV_LINE:
            if (j->stdin_w >= 0) {
                write_all(j->stdin_w, sh->ready_line, strlen(sh->ready_line));
                write_all(j->stdin_w, "\n", 1);
            }
            free(sh->ready_line);
            sh->ready_line = NULL;
            break;
        case EV_INTR:
            sh_forward_output(sh);
            sh_out(sh, "^C\n");
            kill(-j->pgid, SIGINT);
            break;
        case EV_SUSP:
            sh_forward_output(sh);
            sh_out(sh, "^Z\n");
            kill(-j->pgid, SIGTSTP);
            break;
        case EV_EOF:
            if (j->stdin_w >= 0) {
                close(j->stdin_w);
                j->stdin_w = -1;
            }
            break;
        case EV_CLOSE:
            if (!closed) {
                kill(-j->pgid, SIGHUP);
                kill(-j->pgid, SIGCONT);
                kill(-j->pgid, SIGKILL);
                closed = 1;
            }
            break;
        default:
            break;
        }
        if (ev != EV_NONE && ev != EV_CLOSE)
            continue;
        shell_wait(sh, NULL, 0, NULL, -1);
    }
    sh_forward_output(sh);
    ed_hide(sh);

    if (j->state == JOB_STOPPED) {
        job_set_current(sh, j);
        job_print(sh, j, 1);
        return 128 + SIGTSTP;
    }
    int st = j->npids ? j->status[j->npids - 1] : 0;
    if (WIFSIGNALED(st) && WTERMSIG(st) == SIGINT)
        sh->interrupted = 1; /* like bash: Ctrl+C abandons the rest of the line */
    if (WIFSIGNALED(st) && WTERMSIG(st) != SIGINT && WTERMSIG(st) != SIGPIPE) {
#ifdef WCOREDUMP
        sh_printf(sh, "%s%s\n", strsignal(WTERMSIG(st)), WCOREDUMP(st) ? " (core dumped)" : "");
#else
        sh_printf(sh, "%s\n", strsignal(WTERMSIG(st)));
#endif
    }
    int status = job_last_status(j);
    job_remove(sh, j);
    return status;
}

void jobs_kill_all(Shell *sh)
{
    jobs_reap(sh);
    for (Job *j = sh->jobs; j; j = j->next) {
        if (j->state == JOB_DONE || j->pgid <= 0)
            continue;
        kill(-j->pgid, SIGHUP);
        kill(-j->pgid, SIGCONT);
    }
    /* give them a moment to exit, then make sure (no zombies are left) */
    for (int tries = 0; tries < 10; tries++) {
        jobs_reap(sh);
        int alive = 0;
        for (Job *j = sh->jobs; j; j = j->next)
            alive += j->state != JOB_DONE;
        if (!alive)
            break;
        usleep(20000);
    }
    for (Job *j = sh->jobs; j; j = j->next) {
        if (j->state == JOB_DONE)
            continue;
        kill(-j->pgid, SIGKILL);
        for (int i = 0; i < j->npids; i++)
            if (!j->finished[i])
                wait_pid_blocking(j->pids[i]);
    }
    while (sh->jobs)
        job_remove(sh, sh->jobs);
}
