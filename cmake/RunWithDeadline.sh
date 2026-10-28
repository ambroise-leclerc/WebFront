#!/bin/sh
# Usage: RunWithDeadline.sh seconds command [arguments...]
# Runs the command in its own process group and, past the deadline, kills the whole group, so a renderer or an X server
# the command started cannot outlive it. Exits with 124 on a timeout (the command was killed), else with its status.
limit=$1
shift
set -m
"$@" &
command_pid=$!
(
    sleep "$limit"
    kill -KILL -- "-$command_pid" 2>/dev/null
) &
watchdog_pid=$!
wait "$command_pid"
status=$?
kill -KILL -- "-$watchdog_pid" 2>/dev/null
# Whatever the command left behind in its group goes with it.
kill -KILL -- "-$command_pid" 2>/dev/null
[ "$status" = 137 ] && exit 124
exit "$status"
