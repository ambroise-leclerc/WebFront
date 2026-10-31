#!/usr/bin/env bash
set -euo pipefail
# Preserve xvfb-run's calling convention for direct CTest invocations. A whole-suite runner owns one dedicated display.
if [[ ${WEBFRONT_TEST_SHARED_DISPLAY:-} == 1 && -n ${DISPLAY:-} ]]; then
    if [[ ${1:-} == -a ]]; then shift; fi
    exec "$@"
fi
exec xvfb-run "$@"
