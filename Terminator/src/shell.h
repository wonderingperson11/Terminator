/*
 * shell.h - the per-tab shell: interpreter state, job table, line editor.
 *
 * One Shell exists per tab.  In thread mode it runs on its own thread; in
 * process mode in its own forked process.  Either way it talks to the GUI
 * only through two pipes (proto.h) and never touches X11.
 *
 * A Shell is "interactive" when it is connected to the GUI.  Subshells
 * (command substitution, scripts, pipeline members that are builtins, the
 * workers of multiWatch, background compound commands) are forked copies
 * with interactive == 0 that write to plain file descriptors 0/1/2.
 */
#ifndef TERMINATOR_SHELL_H
#define TERMINATOR_SHELL_H

#include "parse.h"
#include "proto.h"
#include "util.h"

#include <signal.h>
#include <sys/types.h>

#define FD_GUI (-2) /* "write this to the GUI as M_OUT" */

/* ---------------------------------------------------------------- vars */
typedef struct Var {
    char *name;
    char *value;
    int exported;
    struct Var *next;
} Var;

/* ---------------------------------------------------------------- jobs */
typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } JobState;

typedef struct Job {
    int id;            /* [n] */
    pid_t pgid;
    pid_t *pids;
    int *status;       /* raw wait status per process */
    int *finished;     /* 1 once reaped (exited or killed) */
    int *stopped;
    int npids;
    JobState state;
    char *cmd;         /* text shown by `jobs` */
    int stdin_w;       /* write end of the job's stdin pipe, or -1 */
    int background;
    int notified;
    struct Job *next;
} Job;

/* ------------------------------------------------------------- editor */
enum { ED_PROMPT, ED_JOB, ED_OFF };          /* what typed input is for   */
enum { SUB_NONE, SUB_SEARCH, SUB_SELECT };    /* prompt-mode sub-states    */

typedef struct {
    int mode;
    int sub;
    char *prompt;      /* current prompt text */
    Buf line;          /* input being edited (UTF-8) */
    size_t cur;        /* byte offset of the cursor */
    int hist_pos;      /* -1: not browsing; else 0 = newest entry */
    char *saved;       /* the line being typed before browsing history */
    /* Ctrl+R */
    Buf search;
    /* Tab completion choice list */
    StrVec choices;
    size_t word_start, word_end;
    char *dir_prefix;
    Buf digits;
    char *sel_saved_line;
    size_t sel_saved_cur;
} Editor;

/* Events returned by shell_poll_input() to the loop that is waiting. */
enum {
    EV_NONE = 0,
    EV_LINE,   /* a line was entered: sh->ready_line */
    EV_INTR,   /* Ctrl+C */
    EV_SUSP,   /* Ctrl+Z */
    EV_EOF,    /* Ctrl+D on an empty line */
    EV_CLOSE,  /* the GUI closed the tab */
};

/* -------------------------------------------------------------- shell */
typedef struct Shell {
    int interactive;
    int process_mode;
    int gui_in, gui_out;   /* message pipes (interactive only) */
    MsgReader rd;
    int wake_slot, wake_fd;/* SIGCHLD self-pipe */
    int out_r, out_w;      /* children's stdout/stderr flow through here */
    int closed;            /* GUI went away */

    int fd_in, fd_out, fd_err; /* where builtins read/write (FD_GUI ok) */

    Var *vars;
    char *arg0;
    char **posv;
    int posc;
    int last_status;
    int subst_ran, subst_status; /* status of the last $(...) */
    pid_t last_bg;
    pid_t shell_pid;

    Job *jobs;
    int cur_job, prev_job;
    int warned_jobs;

    int exit_requested;
    int exit_code;
    volatile int interrupted;
    int cols, rows;

    char *main_prompt;
    Editor ed;
    char *ready_line;
    Buf pending;           /* accumulated multi-line input */
    int source_depth;
} Shell;

/* shell.c ------------------------------------------------------------ */
Shell *shell_new_interactive(int gui_in, int gui_out, int process_mode, const char *start_dir);
Shell *shell_new_batch(void);
void shell_free(Shell *sh);
void shell_interactive_loop(Shell *sh);
int shell_run_string(Shell *sh, const char *src, const char *name);
void shell_enter_subshell(Shell *sh);
pid_t shell_fork(Shell *sh);
_Noreturn void shell_exit_child(Shell *sh, int status);

void sh_write(Shell *sh, int fd, const char *s, size_t n);
void sh_out(Shell *sh, const char *s);
void sh_printf(Shell *sh, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sh_errorf(Shell *sh, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sh_forward_output(Shell *sh); /* drain children's output to the GUI */
void sh_send_title(Shell *sh);

int shell_poll_input(Shell *sh);   /* process queued GUI messages */
int shell_wait(Shell *sh, int *fds, int nfds, short *revents, int timeout_ms);

/* wake.c ------------------------------------------------------------- */
void wake_install_handler(void);
int wake_register(int *read_fd);
void wake_unregister(int slot);
void wake_drain(int fd);
void wake_reset_child(void);

/* vars.c ------------------------------------------------------------- */
const char *var_get(Shell *sh, const char *name);
void var_set(Shell *sh, const char *name, const char *value, int export_flag);
void var_unset(Shell *sh, const char *name);
void var_export(Shell *sh, const char *name);
void vars_init_from_env(Shell *sh, char **envp);
void vars_free(Shell *sh);
char **vars_build_env(Shell *sh, StrVec *extra);
Var *vars_copy(Var *v);

/* expand.c ----------------------------------------------------------- */
enum { EXP_NOSPLIT = 1, EXP_NOGLOB = 2 };
int expand_word(Shell *sh, const char *raw, StrVec *out, int flags);
char *expand_str(Shell *sh, const char *raw); /* no splitting, no globbing */
char *command_subst(Shell *sh, const char *src);

/* exec.c ------------------------------------------------------------- */
int exec_node(Shell *sh, Node *n);
_Noreturn void exec_external(Shell *sh, char **argv, StrVec *env_extra);
int run_script_file(Shell *sh, const char *path, char **args, int nargs, int in_place);
_Noreturn void subshell_run_string(Shell *sh, const char *src);
int wait_pid_blocking(pid_t pid);
int status_to_code(int st);

/* jobs.c ------------------------------------------------------------- */
Job *job_new(Shell *sh, const char *cmd);
void job_add_pid(Job *j, pid_t pid);
void job_free(Job *j);
void job_remove(Shell *sh, Job *j);
void jobs_reap(Shell *sh);
int job_wait_fg(Shell *sh, Job *j);
void jobs_notify(Shell *sh);
Job *job_find(Shell *sh, const char *spec, int *err);
void job_set_current(Shell *sh, Job *j);
void jobs_kill_all(Shell *sh);
void job_print(Shell *sh, Job *j, int with_state);
int job_last_status(Job *j);
int jobs_stopped_count(Shell *sh);

/* builtins.c --------------------------------------------------------- */
typedef int (*BuiltinFn)(Shell *sh, int argc, char **argv, char **raw);
BuiltinFn builtin_lookup(const char *name);
int builtin_must_run_in_shell(const char *name);

/* multiwatch.c ------------------------------------------------------- */
int builtin_multiwatch(Shell *sh, int argc, char **argv, char **raw);

/* editor.c ----------------------------------------------------------- */
void ed_init(Editor *ed);
void ed_free(Editor *ed);
void ed_begin(Shell *sh, int mode, const char *prompt);
void ed_redraw(Shell *sh);
void ed_hide(Shell *sh);
int ed_handle(Shell *sh, int action, const char *text, size_t tlen);
void ed_commit_line(Shell *sh, const char *prompt, const char *text, const char *suffix);

/* history.c ---------------------------------------------------------- */
int hist_init(void);
void hist_add(const char *cmd);
int hist_count(void);
char *hist_get_recent(int back); /* back = 0 is newest; heap copy or NULL */
void hist_show(Shell *sh, int max);
void hist_clear(void);
void hist_search(Shell *sh, const char *term);
void hist_lock(void);
void hist_unlock(void);

/* complete.c --------------------------------------------------------- */
void complete_word(Shell *sh);
void complete_select_key(Shell *sh, int action, const char *text, size_t tlen);

#endif
