#!/bin/sh
# Usage: RunWithDeadline.sh seconds command [arguments...]
# Runs the command in its own process group and, past the deadline, kills the whole group, so a renderer or an X server
# the command started cannot outlive it. Exits with 124 on a timeout (the command was killed), else with its status.
limit=$1
shift
set -m
expired=$(mktemp -u)
"$@" &
command_pid=$!
(
    sleep "$limit" &
    sleeper=$!
    trap 'kill "$sleeper" 2>/dev/null; exit 0' TERM
    wait "$sleeper" || exit 0
    : > "$expired"
    kill -KILL -- "-$command_pid" 2>/dev/null || kill -KILL "$command_pid" 2>/dev/null
# The watchdog must not hold the caller's output pipes: the caller waits for them to close, so a sleeping watchdog would
# delay a command that already finished until the deadline (30 s per Jasmine case under dash, which has no job control here).
) >/dev/null 2>&1 &
watchdog_pid=$!
wait "$command_pid"
status=$?
kill -TERM "$watchdog_pid" 2>/dev/null
# Whatever the command left behind in its group goes with it.
kill -KILL -- "-$command_pid" 2>/dev/null
# Only the watchdog's flag makes a kill a timeout: an unrelated SIGKILL keeps its own status.
if [ -e "$expired" ]; then
    rm -f "$expired"
    exit 124
fi
exit "$status"
