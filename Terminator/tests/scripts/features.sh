#!/bin/bash
# Exercises the script features Terminator interprets itself:
# comments, variables, arguments, $?, pipes, redirection, command
# substitution, nested if/elif/else with [ ] tests, and exit.

GREETING="hello"
echo "$GREETING from $0 with $# argument(s): $1 $2"

# pipes + redirection + command substitution
ls / | head -3 > /tmp/terminator_features_$$.txt
LINES_WRITTEN=$(wc -l < /tmp/terminator_features_$$.txt)
echo "wrote $LINES_WRITTEN lines"
rm -f /tmp/terminator_features_$$.txt

# nested if / elif / else
if [ $# -eq 0 ]; then
    echo "no arguments"
elif [ "$1" = "deep" ]; then
    if [ -d / ]; then
        echo "nested: / is a directory"
    else
        echo "nested: impossible"
    fi
else
    echo "first argument is $1"
fi

false
echo "status after false: $?"
exit 7
echo "never printed"
