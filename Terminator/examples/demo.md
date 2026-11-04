# Demo commands

A walk-through of every task. Start Terminator in either mode:

```bash
make && ./terminator                  # thread mode
./terminator useProcess=True          # process mode
```

Set up a scratch directory first:

```
mkdir -p /tmp/demo/mydir && cd /tmp/demo/mydir
touch abc.txt abcd.txt def.txt
printf 'int main(void){return 0;}\n' > myprog.c
printf 'banana\napple\ncherry\n' > somefile.txt
cp ~/path/to/Terminator/tests/scripts/check.sh .
```

## Task 1: GUI and tabs
* `Ctrl+Shift+T` (or click `[ + ]`) opens a tab in the same directory;
  `Ctrl+PageUp/PageDown`, `Alt+1..9` or a click switch; `Ctrl+Shift+W` closes.
* Start `sleep 30; echo finished` in tab 1, switch to tab 2: tab 1 keeps
  running and shows `finished` when you come back.
* `seq 1 5000`, then `Shift+PageUp` / mouse wheel to scroll back.

## Task 2: builtins and external commands
```
cd /tmp/demo/mydir
gcc -o myprog myprog.c
./myprog
notacommand
echo $?
```

## Task 3: multi-line input and quoting
```
echo "Isn't process creation exciting?\
Fork, exec, repeat!"
echo "Files in $(basename $(pwd)): $(ls | \
wc -l)"
echo 'literal: $HOME' "expanded: $HOME"
echo `echo outer \`echo inner\``
```

## Task 4 & 5: redirection and pipes
```
sort < somefile.txt
ls > abc
./myprog < somefile.txt > outfile.txt
sort < missing.txt
ls *.txt | wc -l
cat somefile.txt | sort | head -2
touch x.tmp y.tmp; ls *.tmp | xargs rm
```

## Task 6: multiWatch
```
multiWatch ["date +%T", "ls | wc -l"]
```
Press `Ctrl+C` to stop it; the prompt comes back.

## Task 7: hotkeys
Type a long line and try `Ctrl+A`, `Ctrl+E`, `←/→`, `Ctrl+W`, `Ctrl+U`,
`Ctrl+K`, `Ctrl+L`, `↑/↓`; select text with the mouse, `Ctrl+Shift+C`, then
`Ctrl+Shift+V`. Rebind a key and restart to see that it persists:
```
bind line-start Ctrl+B
bind line-end Ctrl+B        # rejected: conflict
bind -R                     # back to defaults
```

## Task 8: signals and job control
```
sleep 100          (press Ctrl+Z)
jobs
bg %1
fg %1              (press Ctrl+C)
sleep 3 &
```

## Task 9: history
```
history
```
Press `Ctrl+R`, type `myprog`, Enter; then `Ctrl+R`, `xyz`, Enter. Commands
typed in one tab appear in another tab's `history` and `↑`.

## Task 10: completion
```
./myprog de<Tab>              -> ./myprog def.txt
./myprog def.txt abc<Tab>     -> 1. abc.txt   2. abcd.txt   (press 1)
```

## Task 11: scripts
```
cat check.sh
./check.sh myprog.c
bash check.sh nothere.c
bash ~/path/to/Terminator/tests/scripts/features.sh deep two
bash ~/path/to/Terminator/tests/scripts/syntax_error.sh
```

## Process-mode isolation
In `./terminator useProcess=True`, open two tabs and run `kill -SEGV $$` in
one: that tab reports the crash, and the other keeps working.
