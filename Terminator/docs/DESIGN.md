# Terminator: Design Document

This document explains how Terminator is put together and why. It covers the
X11 GUI and event loop, the two execution modes and how shared state is
synchronised, process and job management, IPC, the data structures behind
history and completion, signal handling, and the other design decisions.

## 1. Architecture at a glance

```
                    +---------------------------------------------+
   X server  <----> |  GUI (gui.c)  -- the only code using Xlib    |
                    |  window, tab bar, per-tab text buffers,      |
                    |  key bindings, selections, poll() loop       |
                    +------+-------------------+-------------------+
                           | M_KEY / M_RESIZE  ^ M_OUT / M_COMMIT /
                           v (pipe)            | M_EDIT / M_TITLE ... (pipe)
          +----------------+------+    +-------+----------------+
          | Shell of tab 1        |    | Shell of tab 2         |   ... one per tab
          | thread  (default) or  |    | thread or process      |
          | forked process        |    |                        |
          | editor, parser,       |    |                        |
          | executor, job table   |    |                        |
          +---+--------------+----+    +------------------------+
              | fork+exec    | stdout/stderr of every child
              v              v (one "output pipe" per tab)
          commands, pipelines, scripts ...

   shared by all tabs:  history ring buffer in shared memory + semaphore
                        ~/.terminator_history, ~/.terminatorrc
```

The key decision is that **a tab is always "a shell at the other end of two
pipes"**. In thread mode the shell is a `pthread`; in process mode it is a
`fork()`ed process. The GUI cannot tell the difference: it writes key events
into one pipe and reads framed display messages from the other. This is why
both modes behave identically, and why the GUI is the only part that touches
X11.

## 2. X11 GUI and event loop (`gui.c`)

**Window.** The GUI opens the display with `XOpenDisplay` and creates a
window with `XCreateSimpleWindow`. It loads a core fixed-width font
(`XLoadQueryFont`, falling back to `fixed`) and registers `WM_DELETE_WINDOW`.
All drawing goes into an off-screen `Pixmap`, which is copied to the window
with `XCopyArea`, so redraws never flicker. Text is drawn with `XDrawString`.
The tab bar, the green prompt, the selection highlight and a block cursor
(hollow when unfocused) are drawn with `XFillRectangle` and
`XDrawRectangle`.

**Per-tab text buffer.** Each tab owns a `TermBuf`:

* `ring`: a ring buffer of 10,000 finished lines (`Line {text, len,
  prompt_start, prompt_len}`). When it is full the oldest line is dropped and
  `base` (the absolute number of the oldest line) is incremented, so
  selections can name lines by an absolute index.
* `partial`: the current unterminated output line, plus a write column, so
  `\r` and `\b` behave like a terminal.
* A small state machine strips ANSI escape sequences (`CSI … final`,
  `OSC … BEL`); `ESC[2J` clears the buffer. A UTF-8 decoder maps code points
  ≤ U+00FF onto the Latin-1 core font and everything else to `?`.

Separately, the tab keeps an **edit area** (prompt, input text, cursor
column) that the shell keeps up to date. Rendering walks the logical lines
*from the bottom up* (edit area first, then scrollback), wraps each one to
the window width, and stops as soon as `scroll + rows` screen rows have been
produced. A redraw therefore costs O(visible rows), however long the
scrollback is. The rows of the last frame are cached for mouse hit-testing.

**Keys.** A `KeyPress` is turned into `(keysym, modifiers)` with
`XLookupString`. Letters are normalised to lower case, and
`ISO_Left_Tab` counts as Shift+Tab. The pair is looked up in the binding table
(`keys.c`). GUI actions (copy, paste, new/close/switch tab, scrolling) are
handled locally. Editing and control actions are sent to the active tab's
shell as `M_KEY(action)`. Printable text is converted from Latin-1 to UTF-8
and sent as `M_KEY(ACT_INSERT, text)`.

**Selections.** A mouse drag selects text and owns `PRIMARY`.
`Ctrl+Shift+C` owns `CLIPBOARD` (with the current input line if nothing is
selected). Terminator answers `SelectionRequest` for `TARGETS`,
`UTF8_STRING`, `STRING` and `TEXT`. Pasting calls `XConvertSelection`,
retries with `STRING` if `UTF8_STRING` is refused, and feeds the text to the
shell as typed input. Each newline becomes an Enter.

**Event loop: no busy-waiting.**

```
loop:
    while XPending(dpy): XNextEvent + handle            # drain Xlib's queue
    redraw if dirty                                       # at most once per wake-up
    if XPending(dpy): continue                            # events read during redraw
    poll([X connection fd] + [each tab's output pipe] + [tab input pipes with queued data])
    read ready tabs (bounded per wake-up), flush queued key events
```

The process sleeps in `poll()` until the X server or a tab has something.
Calling `XPending` before blocking matters: Xlib may already have read events
into its own queue, and `poll()` on the socket would not see them. Reading a
tab is limited to a few buffers per wake-up, so a flood of output (`yes`) can
never starve keyboard events, and Ctrl+C still gets through. Writes to a
tab's input pipe are non-blocking and queued (`TabConn.txq`), so a busy shell
can never block the GUI.

## 3. Thread mode vs process mode, and shared state

| | Thread mode (default) | Process mode (`useProcess=True`) |
|---|---|---|
| tab created by | `pthread_create` | `fork()` |
| GUI ↔ tab | two pipes | two pipes (same code) |
| working directory | per thread, via `unshare(CLONE_FS)` | per process |
| variables, jobs, editor | per `Shell` struct | per process |
| history | shared memory + process-shared semaphore | same segment, inherited over `fork()` |
| crash isolation | no (one process) | yes: the GUI sees EOF, reaps the child and marks the tab dead; others continue |

**Per-tab state in thread mode.** Everything a shell owns lives in its
`Shell` struct: variables (a private table, never `setenv()`), positional
parameters, job table, editor state and pipes. The environment for a child is
built from the exported variables only inside the forked child, just before
`execve()`. The one per-process kernel resource that would otherwise leak
between tabs is the **current directory**. Each tab thread therefore calls
`unshare(CLONE_FS)` on start, which gives the thread its own `fs_struct`
(cwd, umask). This is the per-thread-cwd technique Samba uses. After that,
`chdir()`, relative `open()`, `glob()` and `opendir()` in one tab never
affect another, and children forked by that thread inherit its directory.

**History: one shared, locked place.** `hist_init()` runs in the GUI process
before any tab exists. It creates a POSIX shared-memory object
(`shm_open` → `ftruncate` → `mmap(MAP_SHARED)` → `shm_unlink`), places a
**process-shared semaphore** in it (`sem_init(&sem, 1, 1)`) and loads the
history file. Threads see the mapping because they share the address space.
Forked tab processes inherit the same `MAP_SHARED` mapping, so all tabs, in
either mode, read and write one ring buffer under one lock.

Robustness details:

* `pthread_atfork` takes the history lock around every `fork()`, so a child
  is never created while another thread is halfway through modifying history.
  The semaphore lives in shared memory, so only the parent releases it after
  the fork.
* `sem_timedwait` with a 2-second limit means a tab process that crashes
  while holding the lock cannot freeze the other tabs.

Other shared files: `~/.terminatorrc` is rewritten atomically
(write a temp file, then `rename`) under `flock`. History lines are appended
with `O_APPEND` under the semaphore.

## 4. Process and job management (`exec.c`, `jobs.c`)

**Parsing.** Input is parsed into an AST (`parse.c`): lists
(`;` `&` newline), and-or chains (`&&` `||`), pipelines (`|`, `!`), simple
commands (assignments, words, redirections) and `if/elif/else/fi`. Words keep
their raw text and are expanded only when executed, so `$?`, variables and
command substitutions always see the current state. The parser reports three
outcomes:

* `PARSE_OK`
* `PARSE_ERROR` (bash-style "syntax error near unexpected token …")
* `PARSE_INCOMPLETE`: the input ended inside a quote, `$(`, backtick, after a
  trailing `\`, `|`, `&&`, or inside an `if`.

The interactive loop answers `PARSE_INCOMPLETE` by showing the `>` prompt and
appending the next line. A script answers it with "unexpected end of file".

**Simple commands.** Words are expanded in the shell. A builtin runs inside
the shell, with redirections applied to the shell's `fd_in`/`fd_out`/`fd_err`
for its duration. Anything else is forked:

```
child:  setpgid(0,0); stdin  <- job's stdin pipe
        stdout, stderr -> the tab's output pipe
        apply redirections with dup2(); reset signal dispositions; execve()
parent: setpgid(pid,pid); register the job; wait for it in the foreground
```

The shell searches `PATH` itself so it can recognise **shell scripts**: a
`#!…bash/sh` shebang, or a text file that `execve()` rejects with `ENOEXEC`.
Scripts are interpreted by Terminator in a forked subshell instead of being
handed to `/bin/bash`. `bash file` / `sh file` / `bash -c` are intercepted
the same way. `source` runs the parsed file inside the current shell.

**Pipelines.** N commands use N−1 `pipe()`s and N `fork()`s. All members join
the process group of the first (`setpgid` is called in both the parent and
the child to avoid the race). Builtins and `if` blocks inside a pipeline run
in the forked child, as in bash.

**Subshells.** Command substitution, scripts, pipeline members that aren't
plain programs, `multiWatch` workers and background compound commands all
run in a forked copy of the shell. `shell_enter_subshell()` turns the copy
into a non-interactive shell: it closes every descriptor ≥ 3 (in thread mode
the process image also holds other tabs' pipes), forgets the job table and
the GUI pipes, and writes to plain fds 0/1/2. A subshell waits with blocking
`waitpid()`. As an optimisation, a subshell whose program is a single
external command `exec`s it directly, so each `multiWatch` command really is
one process.

**Jobs.** A `Job` holds the process group, the pids, a raw wait status per
pid, the command text, and the write end of the job's stdin pipe. The
foreground wait (`job_wait_fg`) is a `poll()` loop over three things:

* the GUI pipe: typed lines are echoed and written to the job's stdin;
  Ctrl+D closes it, Ctrl+C sends `kill(-pgid, SIGINT)` and Ctrl+Z sends
  `kill(-pgid, SIGTSTP)`;
* the SIGCHLD wake pipe;
* the tab's output pipe, whose data is forwarded to the GUI.

State changes are collected with
`waitpid(pid, WNOHANG | WUNTRACED | WCONTINUED)` on **this shell's own
pids**. A shell never waits on `-1`, so tabs in one process never reap each
other's children. A stopped job prints `[1]+  Stopped …`. `bg` and `fg` send
`SIGCONT` to the group. Finished background jobs print
`[n]+  Done …` as soon as they are reaped. When a tab closes, its jobs get
`SIGHUP` + `SIGCONT`, then `SIGKILL` if needed, and are reaped so that no
zombies are left behind. Ctrl+C that kills a foreground job also abandons the
rest of the command line (like bash).

## 5. IPC: pipes, `dup2`, redirection

* **GUI ↔ tab:** two pipes per tab carrying framed messages
  (`MsgHdr {type, len}` + payload, `proto.h`). They are read through an
  incremental `MsgReader`, so partial frames are fine. Messages:
  * `M_KEY(action[, text])` and `M_RESIZE(cols, rows)`, GUI → shell;
  * `M_OUT(bytes)`: program output;
  * `M_COMMIT(prompt_len, line)`: a finished input line, drawn with a green
    prompt;
  * `M_EDIT(prompt_len, cursor, text)`: the live edit line;
  * `M_CLEAR`, `M_TITLE(cwd)`, `M_BINDRELOAD`, `M_EXIT(status)`.
* **Command output:** every tab has one persistent *output pipe*. All
  children's stdout and stderr are `dup2`ed onto its write end; the shell
  reads the other end and forwards it as `M_OUT`. Because the shell holds the
  write end for good, job completion is detected with `waitpid`, not with
  EOF. Before printing builtin output or a prompt, the shell drains this pipe
  so ordering is preserved.
* **Command input:** each interactive job gets its own stdin pipe. The write
  end is kept in the `Job`, so typed lines reach whichever job is in the
  foreground, also after `fg`.
* **Redirection** is done in the child with `open()` + `dup2()`: `<`, `>`,
  `>>`, `n>`, `n>&m`, `&>`, `>&file`.
  * A missing input file makes the child print
    `terminator: file: No such file or directory` and `_exit(1)` without
    running the command.
  * Each redirection performs its own `open()`, so every job has its own open
    file description and offset. Background jobs reading the same file at the
    same time cannot disturb each other.
* **Close-on-exec everywhere:** every internal descriptor is created with
  `O_CLOEXEC` (`pipe2`/`open`), so programs inherit only fds 0, 1 and 2. In
  thread mode this is essential: otherwise a child of tab A could hold tab B's
  job stdin pipe open, and B's job would never see EOF.

## 6. Data structures for history and auto-completion

**History** (`history.c`) is a fixed-size ring buffer in shared memory:
`count`, `head`, `total` and `char e[10000][2048]`.

* Adding a command is O(1) and appends one line to the history file. When the
  file grows past 11,000 lines, it is rewritten from the ring (temp file +
  `rename`), which keeps the last 10,000.
* `history` prints the newest ≤ 1,000 entries, numbered by `total`.
* ↑/↓ index the ring from the newest end.

**Ctrl+R** works as follows:

1. Scan newest → oldest for an entry equal to the term.
2. Otherwise compute, for every entry, the length of its *longest common
   substring* with the term. This uses dynamic programming with two rolling
   rows, O(|entry|·|term|) time and O(|term|) memory.
3. Print every distinct entry that reaches the maximum, newest first, if that
   maximum is > 2. Otherwise print "No match for search term in history".

For example, `myprog` prints both `./myprog < in.txt` and
`gcc -o myprog myprog.c`.

**Completion** (`complete.c`) works on the editor's line buffer:

1. Find the word before the cursor: scan back to an unescaped blank or shell
   operator, then undo quoting.
2. Split it into a directory part and a prefix.
3. `opendir()` the directory (relative to the tab's own cwd), collect the
   names that start with the prefix into a sorted `StrVec`, and mark
   directories with `/`.

Then:

* One match replaces the word, with special characters backslash-escaped.
* Several matches first extend the word to their longest common prefix. If
  that still leaves several candidates, the shell prints a numbered list and
  enters a selection sub-mode. Digits pick an entry (decided as soon as the
  number cannot grow into another valid choice, or with Enter); any other key
  cancels.
* No match does nothing.

The rest of the line is never touched.

**Line editor** (`editor.c`): a UTF-8 `Buf` plus a byte cursor that moves by
whole characters. It has three modes: `ED_PROMPT` (full editing, history,
completion, search), `ED_JOB` (cooked input for a running program) and
`ED_OFF` (multiWatch). Two sub-states sit on top of prompt mode: `SEARCH`
and `SELECT`.

## 7. Signal handling

* **Terminator itself** ignores `SIGINT`, `SIGQUIT`, `SIGTSTP`, `SIGTTIN`,
  `SIGTTOU` and `SIGPIPE`, so it can never be interrupted or stopped by
  job-control signals, and a dead tab's pipe can't kill it. Every child
  restores the defaults and an empty signal mask in `shell_fork()` before
  running anything, because ignored dispositions survive `execve()`.
* **Keys are not signals.** Ctrl+C and Ctrl+Z arrive as X11 `KeyPress`
  events. The GUI sends them to the tab's shell, which delivers them itself
  with `kill(-pgid, SIGINT | SIGTSTP)` to its foreground job only. Other
  tabs' jobs are never touched. In `multiWatch`, Ctrl+C `kill()`s every
  worker's process group, reaps them and returns to the prompt.
* **SIGCHLD with many shells per process.** A signal is delivered to an
  arbitrary thread, so the handler cannot know which tab it concerns.
  `wake.c` gives every shell a slot with a non-blocking self-pipe. The handler
  (async-signal-safe: it only calls `write`) writes one byte to every active
  slot, and each shell's `poll()` wakes up and reaps only its own pids with
  `WNOHANG`. Slot pipes are never closed, so the handler can never write into
  a recycled descriptor. `SA_RESTART` is set and every `poll`/`read`/`write`
  loop tolerates `EINTR`. `SA_NOCLDSTOP` is deliberately *not* set, so a
  Ctrl+Z stop wakes the shell.
* `SIGTSTP` sent with `kill()` is honoured because the job's process group
  is not orphaned: its parent, the shell, is in another group of the same
  session.

## 8. Other design decisions

* **Word expansion** (`expand.c`) builds each field twice: the literal value,
  and a glob pattern in which quoted characters are backslash-escaped. Quoted
  `*` therefore never globs, while unquoted expansion results are split on
  blanks and globbed, as in bash. `"$@"` expands to one field per argument.
  It supports `$VAR`, `${VAR}`, `${#VAR}`, `${VAR:-w}`, `${VAR:=w}`,
  `${VAR:+w}`, `$?`, `$#`, `$$`, `$!`, `$0…$9`, `$@`, `$*` and `~`/`~user`.
  Command substitution runs in a forked subshell (innermost first, since the
  inner text is expanded while the outer runs) and strips trailing newlines.
  In an interactive shell it gets its own process group, so Ctrl+C can stop a
  long substitution.
* **Backslash-newline** outside quotes joins lines. Inside double quotes it
  keeps the newline: the handout's example requires `Isn't process creation
  exciting?` and `Fork, exec, repeat!` on separate lines.
* **multiWatch** keeps per-command state `{pid, read fd, pending output,
  timestamp, next start time}`. The poll timeout is the nearest deadline:
  either a rerun 2 s after a command finished, or a 60 ms "settle" timer.
  Output arriving within 60 ms of the previous chunk is printed as one block,
  so a command's output isn't split into fragments; very long output is
  flushed every 64 KB. Each block is printed in the required
  `"cmd", <unix_timestamp>:` + dashes format.
* **Key bindings** (`keys.c`) map 23 named actions to `(modifiers, keysym)`.
  The rc file is parsed over the defaults with these rules:
  * unknown actions, unparseable keys and keys that would shadow normal typing
    (`Return`, `BackSpace`, or a printable key without Ctrl/Alt) are
    rejected;
  * when two actions share a key, the later custom one reverts to its default,
    repeating until no conflicts remain.

  Problems are shown in the tab and on stderr. The `bind` builtin validates
  against the current set, rewrites the file and sends `M_BINDRELOAD`, so
  changes apply immediately in both modes without recompiling.
* **Testing aids.** `--headless` runs the real tab/shell code (thread or
  process mode, several tabs) with stdin/stdout instead of a window. `-c` and
  `--script` run the interpreter alone. `tests/run_tests.sh` uses them for
  ~110 automated checks.
