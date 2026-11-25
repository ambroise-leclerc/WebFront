#!/bin/sh
# Stands in for the application under test (CheckRealXdg.cmake): writes the XDG roots it sees to $XDG_SEEN, one line
# each, then answers as each helper expects: a passing Jasmine report in $XDG_REPORT/report.json when it is set, and the
# start-up milestones RunStartupTime.cmake reads.
: > "$XDG_SEEN"
for name in XDG_DATA_HOME XDG_CACHE_HOME WEBFRONT_TEST_REAL_XDG_DATA_HOME WEBFRONT_TEST_REAL_XDG_CACHE_HOME; do
    if eval "[ \"\${$name+set}\" = set ]"; then
        eval "value=\$$name"
        printf '%s=[%s]\n' "$name" "$value" >> "$XDG_SEEN"
    else
        printf '%s absent\n' "$name" >> "$XDG_SEEN"
    fi
done
if [ -n "$XDG_REPORT" ]; then
    printf '{"status":"passed"}\n' > "$XDG_REPORT/report.json"
fi
echo 'startup-time: ready 1 ms'
echo 'WebFront startup: window shown at 1 ms' >&2
