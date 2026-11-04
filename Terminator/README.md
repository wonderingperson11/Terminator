# Terminator

**Terminator** is a terminal emulator *and* shell written in C for Linux
(Computing Lab CS69201 term project). It draws its own X11 window with Xlib,
supports multiple tabs, and runs commands with pipes, I/O redirection, job
control, a shared searchable history, file-name completion, configurable
hotkeys, a `multiWatch` builtin and its own interpreter for bash scripts.

```
[ 1: ~/Downloads ] [ 2: /tmp ] [ + ]
user@terminator> ls
abc.txt  def.txt  myprog.c
user@terminator> echo "Files in $(basename $(pwd)): $(ls | \
> wc -l)"
Files in Downloads: 3
```

Each tab is an independent shell. Tabs run as **threads** (default) or as
**separate processes** (`useProcess=True`). Both modes behave the same: `cd`
in one tab never moves another, Ctrl+C only touches the active tab's jobs, and
a command typed in one tab shows up in every tab's history.

---

## Dependencies

| What | Debian / Ubuntu | Fedora | Arch |
|------|-----------------|--------|------|
| C compiler + make | `build-essential` | `gcc make` | `base-devel` |
| Xlib headers and library | `libx11-dev` | `libX11-devel` | `libx11` |
| (optional) nicer bitmap fonts | `xfonts-base` | `xorg-x11-fonts-misc` | `xorg-fonts-misc` |

```bash
sudo apt install build-essential libx11-dev
```

Nothing else is needed: no toolkit and no font library. Terminator uses the
core X fonts. It tries `9x15`-sized "fixed" first and falls back to `fixed`,
which every X server has. To pick another font, set `TERMINATOR_FONT`, e.g.
`TERMINATOR_FONT=10x20 ./terminator`.

## Building

```bash
make
```

This produces `./terminator`. The Makefile gets the X11 flags from
`pkg-config x11` (falling back to `-lX11`) and links with `-pthread -lrt`.
`make clean` removes the build products.

## Running

```bash
./terminator
```

Thread mode (default): one process, and every tab is a POSIX thread.

```bash
./terminator useProcess=True
```

Process mode: the GUI forks one process per tab. A tab that crashes or is
closed cannot affect the others.

`useProcess=False` selects thread mode explicitly. The window title shows
`[process mode]` when process mode is active.

### Tabs and keys

| Key | Action |
|-----|--------|
| `Ctrl+Shift+T` or clicking `[ + ]` | new tab (opens in the current tab's directory) |
| `Ctrl+Shift+W`, middle/right-click on a tab, `exit`, `Ctrl+D` | close tab |
| `Ctrl+PageDown` / `Ctrl+PageUp`, `Ctrl+Tab` / `Ctrl+Shift+Tab`, `Alt+1..9`, click | switch tab |
| `Shift+PageUp` / `Shift+PageDown`, mouse wheel | scroll back / forward |
| `←` / `→`, `Home` / `End` | move within the line |
| `Ctrl+A` / `Ctrl+E` | start / end of the line |
| `↑` / `↓` | browse history |
| `Ctrl+U` / `Ctrl+K` | delete to start / end of the line |
| `Ctrl+W` | delete the previous word |
| `Ctrl+L` | clear the screen |
| `Ctrl+D` | exit the shell (close tab) on an empty line, or send EOF to the running command |
| `Ctrl+C` | interrupt the running command (or discard the line) |
| `Ctrl+Z` | suspend the running command |
| `Ctrl+R` | search history (`Enter search term:`) |
| `Tab` | complete a file name |
| `Ctrl+Shift+C` / `Ctrl+Shift+V` | copy / paste via the X11 CLIPBOARD |
| mouse drag / middle click / `Shift+Insert` | select (PRIMARY) / paste PRIMARY |

### Changing key bindings

Every key in the hotkey table (and the tab/scroll keys) can be rebound
without recompiling. Bindings live in `~/.terminatorrc` (the `TERMINATORRC`
environment variable overrides the path), so they survive restarts:

```
# ~/.terminatorrc
bind line-start Ctrl+B
bind history-search Alt+R
```

You can also use the `bind` builtin inside Terminator. It validates the key,
saves it to `~/.terminatorrc` and makes the GUI reload the file immediately:

```
user@terminator> bind                        # list the current bindings
user@terminator> bind -l                     # list action names
user@terminator> bind line-start Ctrl+B      # rebind
user@terminator> bind -r line-start          # restore one default
user@terminator> bind -R                     # restore all defaults
```

Invalid keys, unknown actions and conflicts (two actions on one key) are
reported, in the window and on stderr, and the action falls back to its
default key. See `examples/terminatorrc` for a commented sample.

### Builtins

`cd`, `pwd`, `echo`, `exit`, `history`, `jobs`, `fg`, `bg`, `kill`
(understands `%n`), `wait`, `source` / `.`, `export`, `unset`, `shift`,
`test` / `[`, `true`, `false`, `:`, `clear`, `bind`, `help`, `multiWatch`.

```
user@terminator> multiWatch ["date +%T", "ls | wc -l"]      # Ctrl+C to stop
user@terminator> multiWatch -n 5 ["uptime"]                   # every 5 s instead of 2 s
```

## Features (by task)

| Task | Status | Notes |
|------|--------|-------|
| 1. X11 GUI and tabs | ✅ | Xlib window (`XOpenDisplay`, `XCreateSimpleWindow`, `XNextEvent`, `XDrawString`), double-buffered; per-tab text buffer with 10,000 lines of scrollback; soft line wrapping; tabs open/close/switch by keyboard and mouse; background tabs keep running; `poll()` event loop, no busy-waiting. |
| 2. Built-in and external commands | ✅ | `fork()` + `execve()` (PATH searched by the shell); `$?`; `terminator: x: command not found`. |
| 3. Multiline input and quoting | ✅ | `\` continuation, multi-line quotes, nested/multi-line `$(...)` and backticks (innermost first), bash quoting rules, `>` continuation prompt; also an unfinished `if`, a trailing `|`, `&&` or `||` continue onto the next line. |
| 4. I/O redirection | ✅ | `<`, `>`, `>>`, `2>`, `2>&1`, `&>` with `dup2()` in the child; a missing input file is reported and the command does not run; every redirection opens its own file description, so concurrent background readers never share an offset. |
| 5. Pipes | ✅ | N commands → N−1 `pipe()`s and N children in one process group; combinable with redirection. |
| 6. multiWatch | ✅ | One process and one pipe per command; `poll()` on all read ends; reruns every 2 s (`-n` to change); exact output format; Ctrl+C from the X11 loop `kill()`s every child. |
| 7. Hotkeys | ✅ | All listed keys; configurable through `~/.terminatorrc` / `bind`; validation with fallback to defaults. |
| 8. Signals and job control | ✅ | Ctrl+C / Ctrl+Z delivered with `kill(-pgid, …)`; `cmd &`, `jobs`, `fg`, `bg`, `kill %n`, `Done` notices; Terminator itself ignores job-control signals and never stops. |
| 9. Searchable history | ✅ | Last 10,000 commands in `~/.terminator_history` (or `$HISTFILE`), shared by all tabs in both modes; `history` shows the most recent 1,000; Ctrl+R: exact match → longest common substring (> 2 chars) → "No match…". |
| 10. File-name completion | ✅ | Single match completes (directories get `/`), several extend to the longest common prefix and then show a numbered list to pick from; no match does nothing; works in subdirectories (`src/ma` + Tab). |
| 11. Bash scripts | ✅ | `./script.sh`, `bash script.sh`, `sh script.sh`, `source script.sh` interpreted by Terminator itself: comments, variables, `$1…`, `$#`, `$?`, `$@`, pipes, redirection, substitution, nested `if/elif/else/fi`, `[ … ]` (`-eq -ne -lt -le -gt -ge = != -z -n -f -d -e -r -w -x -s`, `!`, `-a`, `-o`), `exit [n]`, `&&`, `||`. The whole script is parsed first, so a syntax error (e.g. a missing `fi`) is reported and nothing runs. |

## Testing

```bash
make test
```

`tests/run_tests.sh` runs about 110 automated checks covering every task, in
thread mode and in process mode. It needs no X server. It drives the real
tab and shell code through two testing aids built into the binary:

* `./terminator --headless [useProcess=True]` runs one or more tabs without a
  window. It reads lines from stdin, types them into the tab and prints what
  the tab would display. Lines like `@INTR`, `@SUSP`, `@EOF`, `@TAB`,
  `@SEARCH`, `@UP`, `@NEWTAB` and `@SWITCH n` send keys or manage tabs (see
  `src/headless.c`).
* `./terminator -c 'command line'` and `./terminator --script file args…`
  run the interpreter without a GUI.

`tests/scripts/` contains sample scripts (`check.sh` from the handout,
`features.sh`, `syntax_error.sh`, `source_me.sh`), and `examples/demo.md` is
a list of commands for a live demonstration.

## Known limitations

* **No pseudo-terminal.** As the handout asks, command output is captured
  through pipes. Programs therefore see a pipe, not a tty: full-screen and
  line-editing programs (`vim`, `top`, `less`, `python` REPL) are not usable,
  colours are off, and `TERM=dumb` and `PAGER=cat` are set. For convenience,
  a bare `ls` writing to the window gets `-C` so it prints columns like bash.
* **Fonts:** core X fonts are 8-bit. UTF-8 text in the Latin-1 range is
  shown correctly, and other characters appear as `?`.
* **Shell language:** loops, functions, `case`, `( … )` subshells, here-documents,
  `$(( … ))` arithmetic and aliases are not implemented. The handout does not
  require them.
* **Thread mode** keeps each tab's working directory separate with Linux's
  `unshare(CLONE_FS)`. Some container sandboxes (e.g. Docker's default seccomp
  profile) forbid it. Terminator then prints a warning and the tabs share one
  directory. Process mode is not affected. Thread mode is a single process by
  design, so `kill -9 $$` or a crash ends every tab; process mode isolates them.
* History entries longer than 2,047 bytes are truncated. In the history file,
  a newline inside a multi-line command is stored as the byte `0x1f`.
* Pastes larger than 4 MB (X11 `INCR` transfers) are not supported.
* `exit` inside a script run with `source` closes the tab, as in bash.
* By design, and to match the handout's example, a backslash-newline *inside
  double quotes* keeps the newline. Bash would join the lines.

## Project layout

```
Makefile
src/main.c        argument parsing, signal dispositions, mode selection
src/gui.c         X11 window, tab bar, rendering, keys, mouse, selections
src/tab.c         spawning a tab as a thread or a process; GUI-side pipes
src/proto.[ch]    message framing between the GUI and a tab's shell
src/shell.c       per-tab shell: read-eval loop, output, wait primitives
src/editor.c      line editor (hotkey actions, history browsing, Ctrl+R)
src/complete.c    Tab completion
src/parse.c       lexer + recursive-descent parser (AST)
src/expand.c      quotes, $VAR, $(...), `...`, ~, splitting, globbing
src/exec.c        pipelines, redirection, fork/exec, script interpreter
src/jobs.c        job table, process groups, fg/bg, Ctrl+C/Ctrl+Z
src/builtins.c    builtin commands, test/[ ], bind
src/multiwatch.c  multiWatch
src/history.c     shared-memory history + semaphore + history file
src/keys.c        key bindings and ~/.terminatorrc
src/wake.c        SIGCHLD self-pipe for many shells in one process
src/vars.c        per-shell variables
src/util.c        buffers, I/O helpers, UTF-8
src/headless.c    window-less driver used by the tests
docs/DESIGN.md    design document
tests/            automated tests and sample scripts
examples/         sample ~/.terminatorrc and demo commands
```

See [docs/DESIGN.md](docs/DESIGN.md) for the design document.
