#!/bin/sh
# Stands in for a hung application that left a child process behind.
sleep 300 &
printf '%s %s\n' "$$" "$!" > "$OUT/pids.txt"
wait
