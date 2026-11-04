#!/usr/bin/env bash
# Automated tests for Terminator.  No X server is needed: the shell is driven
# through `terminator --headless` (the same tab/shell code the GUI uses, in
# thread and in process mode) and `terminator -c`.
#
#   make test        or        ./tests/run_tests.sh

cd "$(dirname "$0")/.." || exit 1
T="$PWD/terminator"
SCRIPTS="$PWD/tests/scripts"
[ -x "$T" ] || { echo "build first: make"; exit 1; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/terminator-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/mydir"
touch "$WORK/mydir/abc.txt" "$WORK/mydir/abcd.txt" "$WORK/mydir/def.txt"
printf 'int main(void){return 0;}\n' > "$WORK/mydir/myprog.c"
printf 'banana\napple\ncherry\n' > "$WORK/mydir/fruits.txt"
export HISTFILE="$WORK/history"
export TERMINATORRC="$WORK/terminatorrc"

pass=0
fail=0

with_timeout() {
    if command -v timeout >/dev/null 2>&1; then
        timeout 60 "$@"
    else
        perl -e 'alarm 60; exec @ARGV' "$@"
    fi
}

# check NAME EXPECTED-SUBSTRING ACTUAL
check() {
    if [[ "$3" == *"$2"* ]]; then
        pass=$((pass + 1))
        echo "ok   - $1"
    else
        fail=$((fail + 1))
        echo "FAIL - $1"
        echo "       expected to contain: $(printf '%q' "$2")"
        echo "       got:"
        printf '%s\n' "$3" | sed 's/^/         /'
    fi
}

# check_not NAME UNEXPECTED-SUBSTRING ACTUAL
check_not() {
    if [[ "$3" != *"$2"* ]]; then
        pass=$((pass + 1))
        echo "ok   - $1"
    else
        fail=$((fail + 1))
        echo "FAIL - $1 (unexpected: $2)"
        printf '%s\n' "$3" | sed 's/^/         /'
    fi
}

batch() { (cd "$WORK/mydir" && with_timeout "$T" -c "$1" 2>&1); }

# headless MODE line...   (MODE is False for threads, True for processes)
headless() {
    local mode=$1
    shift
    (cd "$WORK/mydir" && printf '%s\n' "$@" | with_timeout "$T" --headless "useProcess=$mode" 2>&1)
}

echo "# Task 2: built-in and external commands"
out=$(batch 'notacommand; echo "status=$?"')
check "unknown command reports an error" "terminator: notacommand: command not found" "$out"
check "\$? holds the exit status" "status=127" "$out"
out=$(batch 'false; echo $?; true; echo $?')
check "\$? after false/true" $'1\n0' "$out"
out=$(batch 'cd /; pwd; cd - >/dev/null; basename $(pwd)')
check "cd and cd -" $'/\nmydir' "$out"

echo "# Task 3: quoting and command substitution"
out=$(batch "echo 'literal: \$HOME' \"expanded: \$HOME\"")
check "single vs double quotes" "literal: \$HOME expanded: $HOME" "$out"
out=$(batch 'echo "Files in $(basename $(pwd)): $(ls | wc -l | tr -d " ")"')
check "nested \$(...)" "Files in mydir: 5" "$out"
out=$(batch 'echo `echo outer \`echo inner\``')
check "nested backticks" "outer inner" "$out"
out=$(batch 'x="a   b"; echo $x; echo "$x"')
check "field splitting vs quoting" $'a b\na   b' "$out"
out=$(batch 'echo *.txt')
check "globbing" "abc.txt abcd.txt def.txt fruits.txt" "$out"
out=$(batch "echo '*.txt' \"*\"")
check "quoted globs stay literal" "*.txt *" "$out"

echo "# Task 4: I/O redirection"
out=$(batch 'sort < fruits.txt')
check "input redirection" $'apple\nbanana\ncherry' "$out"
out=$(batch 'ls > listing.out; cat listing.out; echo extra >> listing.out; tail -1 listing.out')
check "output redirection and append" $'abc.txt' "$out"
check "append" "extra" "$out"
out=$(batch 'sort < fruits.txt > sorted.out; cat sorted.out')
check "< and > together" $'apple\nbanana\ncherry' "$out"
out=$(batch 'sort < missing.txt; echo "rc=$?"')
check "missing input file is reported" "terminator: missing.txt: No such file or directory" "$out"
check "command not run on missing input" "rc=1" "$out"
out=$(batch 'ls nothere 2> err.out; grep -c nothere err.out')
check "2> redirection" "1" "$out"
out=$(batch 'cat fruits.txt > r1.out & cat fruits.txt > r2.out & wait; cmp r1.out r2.out && echo same')
check "background jobs reading the same file" "same" "$out"

echo "# Task 5: pipes"
out=$(batch 'ls *.txt | wc -l | tr -d " "')
check "two-stage pipe" "4" "$out"
out=$(batch 'cat fruits.txt | sort | head -2 | tail -1')
check "four-stage pipe" "banana" "$out"
out=$(batch 'touch del1.tmp del2.tmp; ls *.tmp | xargs rm; ls *.tmp 2>&1 | grep -c "No such"')
check "pipe into xargs rm" "1" "$out"
out=$(batch 'sort < fruits.txt | head -1 > first.out; cat first.out')
check "pipe combined with redirection" "apple" "$out"

echo "# Task 11: bash scripts (interpreted, not run by /bin/bash)"
cp "$SCRIPTS"/*.sh "$WORK/mydir/"
out=$(batch './check.sh myprog.c; ls myprog')
check "./check.sh (file exists)" "File exists, compiling..." "$out"
check "script ran gcc" "myprog" "$out"
out=$(batch 'bash check.sh nothere.c')
check "bash check.sh (file missing)" "Source file missing!" "$out"
out=$(batch './features.sh deep two; echo "exit=$?"')
check "script arguments" "hello from ./features.sh with 2 argument(s): deep two" "$out"
check "script pipes/redirection/subst" "wrote" "$out"
check "nested if/elif" "nested: / is a directory" "$out"
check "\$? in scripts" "status after false: 1" "$out"
check "exit n" "exit=7" "$out"
check_not "nothing after exit" "never printed" "$out"
out=$(batch './syntax_error.sh; echo "exit=$?"')
check "syntax error reported" "syntax error: unexpected end of file" "$out"
check_not "nothing runs on a syntax error" "must not run" "$out"
out=$(batch 'source source_me.sh; echo "$SOURCED_VAR / $(pwd)"')
check "source runs in the current shell" "set by source_me.sh / /" "$out"
out=$(batch 'printf "echo from-sh-c\n" > nos.sh; sh nos.sh; bash -c "echo dash-c \$0" zero')
check "sh file / bash -c" $'from-sh-c\ndash-c zero' "$out"

echo "# Task 9: history file"
: > "$HISTFILE"
for i in $(seq 1 10500); do echo "cmd$i"; done > "$HISTFILE"
batch 'true' >/dev/null
check "history file trimmed to 10000" "10000" "$(wc -l < "$HISTFILE" | tr -d ' ')"
out=$(batch 'history | wc -l | tr -d " "')
check "history shows the most recent 1000" "1000" "$out"
check "history file keeps the newest" "cmd10500" "$(tail -1 "$HISTFILE")"
printf '%s\n' 'gcc -o myprog myprog.c' 'ls -l' './myprog < in.txt' 'echo hi' > "$HISTFILE"

for mode in False True; do
    label="thread"
    [ "$mode" = True ] && label="process"
    echo "# Interactive features ($label mode)"

    out=$(headless $mode 'echo "Isn'"'"'t process creation exciting?\' 'Fork, exec, repeat!"')
    check "[$label] continuation inside double quotes" $'> Fork, exec, repeat!"\nIsn\'t process creation exciting?\nFork, exec, repeat!' "$out"
    out=$(headless $mode 'echo "Files in $(basename $(pwd)): $(ls *.txt | \' 'wc -l | tr -d " ")"')
    check "[$label] multi-line \$(...)" "Files in mydir: 4" "$out"
    out=$(headless $mode 'if [ -f abc.txt ]' 'then echo found' 'fi')
    check "[$label] multi-line if" $'> fi\nfound' "$out"
    out=$(headless $mode "echo 'one" "two'")
    check "[$label] single quotes across lines" $'one\ntwo' "$out"

    out=$(headless $mode '@NOWAIT sleep 100' '@SLEEP 300' '@SUSP' 'jobs' 'bg %1' 'jobs' '@NOWAIT fg %1' '@SLEEP 300' '@INTR' 'echo "rc=$?"')
    check "[$label] Ctrl+Z stops" "[1]+  Stopped                 sleep 100" "$out"
    check "[$label] bg" "[1]+ sleep 100 &" "$out"
    check "[$label] jobs shows running" "[1]+  Running                 sleep 100 &" "$out"
    check "[$label] fg + Ctrl+C" $'sleep 100\n^C' "$out"
    check "[$label] \$? after Ctrl+C" "rc=130" "$out"
    out=$(headless $mode 'sleep 1 &' '@SLEEP 1600' 'echo after')
    check "[$label] background job + Done notice" "[1]+  Done                    sleep 1" "$out"
    out=$(headless $mode '@NOWAIT cat' '@SLEEP 200' '@NOWAIT typed line' '@SLEEP 200' '@EOF' '@WAIT' 'echo ok')
    check "[$label] typed input reaches the job; Ctrl+D sends EOF" $'typed line\ntyped line' "$out"
    out=$(headless $mode '@NOWAIT sleep 5; echo NOT"PRINTED"' '@SLEEP 300' '@INTR' 'echo next')
    check_not "[$label] Ctrl+C abandons the rest of the line" "NOTPRINTED" "$out"

    out=$(headless $mode '@NOWAIT multiWatch ["echo first", "ls *.txt | wc -l"]' '@SLEEP 2600' '@INTR' 'echo back')
    check "[$label] multiWatch header" '"echo first", ' "$out"
    check "[$label] multiWatch separator" "----------------------------------------------------" "$out"
    check "[$label] multiWatch pipeline output" '"ls *.txt | wc -l", ' "$out"
    runs=$(printf '%s\n' "$out" | grep -c '^first$')
    check "[$label] multiWatch reruns every 2s" "yes" "$([ "$runs" -ge 2 ] && echo yes)"
    check "[$label] Ctrl+C ends multiWatch" $'^C\n'"$(id -un)@terminator> echo back" "$out"
    check "[$label] shell alive after multiWatch" "back" "$out"

    out=$(headless $mode '@TYPE cat de' '@TAB' '@ENTER')
    check "[$label] Tab: single match" "> cat def.txt" "$out"
    out=$(headless $mode '@TYPE ls abc' '@TAB' '@TYPE 2' '@ENTER')
    check "[$label] Tab: numbered list" "1. abc.txt   2. abcd.txt" "$out"
    check "[$label] Tab: pick from list" "> ls abcd.txt" "$out"
    out=$(headless $mode '@TYPE ./nothere fru' '@TAB' '@ENTER')
    check "[$label] Tab: rest of line kept" "> ./nothere fruits.txt" "$out"

    printf '%s\n' 'gcc -o myprog myprog.c' 'ls -l' './myprog < in.txt' 'echo hi' > "$HISTFILE"
    out=$(headless $mode '@SEARCH' 'myprog')
    check "[$label] Ctrl+R prompt" "Enter search term: myprog" "$out"
    check "[$label] Ctrl+R longest common substring" $'./myprog < in.txt\ngcc -o myprog myprog.c' "$out"
    out=$(headless $mode '@SEARCH' 'ls -l')
    check "[$label] Ctrl+R exact match" $'Enter search term: ls -l\nls -l' "$out"
    out=$(headless $mode '@SEARCH' 'xyz')
    check "[$label] Ctrl+R no match" "No match for search term in history" "$out"
    out=$(headless $mode '@UP' '@UP' '@ENTER')
    check "[$label] Up arrow recalls history" "> ./myprog < in.txt" "$out"

    out=$(headless $mode 'cd /' 'echo tab1' '@NEWTAB' 'pwd' 'history 2' '@SWITCH 1' 'pwd')
    if [ "$mode" = True ] || [ "$(uname -s)" = Linux ]; then
        check "[$label] cd in one tab does not move another" "$(cd "$WORK/mydir" && pwd -P)" "$out"
    fi
    check "[$label] history shared between tabs" "echo tab1" "$out"
    out=$(headless $mode 'exit 3'; echo "status=$?")
    check "[$label] exit closes the tab" "status=3" "$out"
done

echo "# Process mode: a crashing tab does not affect the others"
out=$(headless True 'echo alive' '@NEWTAB' 'kill -SEGV $$' '@SLEEP 300' '@SWITCH 1' 'echo still-alive')
check "crashed tab reported" "[tab process killed by signal 11]" "$out"
check "other tab keeps working" "still-alive" "$out"

echo "# Task 7: configurable key bindings"
rm -f "$TERMINATORRC"
out=$(batch 'bind line-start Ctrl+B')
check "bind saves a custom key" "line-start is now bound to Ctrl+B" "$out"
check "binding persisted" "bind line-start Ctrl+B" "$(cat "$TERMINATORRC")"
out=$(batch 'bind line-end Ctrl+B')
check "conflicting bind rejected" "already bound to 'line-start'" "$out"
out=$(batch 'bind line-end Ctrl+Nope')
check "invalid key rejected" "unknown key" "$out"
printf 'bind kill-line-start Ctrl+A\nbind bogus Ctrl+Q\nbind eof x\n' > "$TERMINATORRC"
out=$(batch 'bind')
check "rc conflict falls back to default" "conflicts with 'line-start'" "$out"
check "rc unknown action reported" "unknown action 'bogus'" "$out"
check "rc invalid key reported" "would shadow normal typing" "$out"
check "defaults kept" "kill-line-start  Ctrl+U" "$out"
out=$(batch 'bind -R; bind')
check "bind -R restores defaults" "line-start       Ctrl+A" "$out"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
