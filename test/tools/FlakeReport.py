#!/usr/bin/env python3
"""Counts the CTest cases that failed in the recent runs of a workflow, per platform job.

Read-only: it lists runs and reads the logs of their failed jobs through the GitHub CLI (`gh`), which must be
authenticated. A case that fails on one platform of a run whose other platforms pass is the usual sign of an
intermittent failure; one that fails everywhere on a branch is more likely a regression of that branch. Every attempt
of a run is read: a job that failed and passed once run again is exactly an intermittent failure.

    python3 test/tools/FlakeReport.py --limit 60
    python3 test/tools/FlakeReport.py --days 7

The report ends with the reruns each job needed before it went green: the failed attempts of a job in runs that finally
passed, so that reliability can be measured from one week to the next (#628).
"""
import argparse
import collections
import json
import re
import datetime
import subprocess
import sys
import time

MAX_WINDOW_RUNS = 1000  # what `gh run list` returns at most

# "644/687 Test #615: DeveloperBrowserIntegration-jasmine .....***Failed   18.97 sec", after the log's own prefix.
# A repeated case (`--repeat until-fail:5`) has no "644/687" before its first attempts.
FAILED_CASE = re.compile(r"(?:\d+/\d+ )?Test +#\d+: (?P<name>.+?) \.*\*\*\*(?P<kind>Failed|Timeout|Exception[^ ]*|Not Run)")


def failed_cases(log):
    """The (case, kind) pairs of a CTest log; cases that did not run because a dependency failed are left out."""
    found = []
    for line in log.splitlines():
        match = FAILED_CASE.search(line)
        if match and match["kind"] != "Not Run":
            found.append((match["name"].strip(), match["kind"]))
    return found


def gh(*arguments):
    # GitHub answers a read with a 502 now and then: three tries, ten seconds apart.
    for attempt in range(3):
        result = subprocess.run(["gh", *arguments], capture_output=True, text=True, encoding="utf-8", errors="replace")
        if result.returncode == 0 or "HTTP 50" not in result.stderr:
            break
        if attempt < 2:
            time.sleep(10)
    if result.returncode != 0:
        raise RuntimeError(f"gh {' '.join(arguments)}: {result.stderr.strip()}")
    return result.stdout


def attempts_to_read(run):
    """The attempts of a run that can hold a failed job: all the earlier ones, and the last one when it failed."""
    last = run.get("attempt") or 1
    return [attempt for attempt in range(1, last + 1) if attempt < last or run["conclusion"] == "failure"]


def within_days(run, days, now=None):
    """Whether the run started in the last `days` days; a run without a date is kept."""
    started = run.get("createdAt")
    if days is None or not started:
        return True
    now = now or datetime.datetime.now(datetime.timezone.utc)
    return now - datetime.datetime.fromisoformat(started.replace("Z", "+00:00")) <= datetime.timedelta(days=days)


def failed_jobs(run, attempt):
    path = f"repos/{{owner}}/{{repo}}/actions/runs/{run['databaseId']}/attempts/{attempt}/jobs?per_page=100"
    return [job for job in json.loads(gh("api", path))["jobs"] if job["conclusion"] in ("failure", "timed_out")]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--workflow", default="BuildAndTest.yml")
    parser.add_argument("--limit", type=int, help="how many of the latest runs to read (60; with --days, every run of the window up to 1000)")
    parser.add_argument("--days", type=float, help="only the runs of the last N days")
    parser.add_argument("--branch", help="only the runs of this branch")
    arguments = parser.parse_args()

    limit = arguments.limit or (MAX_WINDOW_RUNS if arguments.days else 60)
    listing = ["run", "list", "--workflow", arguments.workflow, "--limit", str(limit),
               "--json", "databaseId,conclusion,headBranch,event,createdAt"]
    if arguments.days:
        # The server applies the window, so that the limit only truncates a window with more runs than the limit.
        since = datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(days=arguments.days)
        listing += ["--created", f">={since:%Y-%m-%dT%H:%M:%SZ}"]
    if arguments.branch:
        listing += ["--branch", arguments.branch]
    runs = json.loads(gh(*listing))
    truncated = len(runs) >= limit
    completed = [run for run in runs if run["conclusion"] in ("success", "failure") and within_days(run, arguments.days)]
    for run in completed:
        # `gh run list` has no attempt field: the run itself says how many times it ran.
        if "attempt" not in run:
            run["attempt"] = int(gh("api", f"repos/{{owner}}/{{repo}}/actions/runs/{run['databaseId']}", "--jq", ".run_attempt"))
    cases = collections.defaultdict(list)
    other = collections.Counter()
    rerun = 0
    reruns_before_green = collections.Counter()
    for run in completed:
        last = run.get("attempt") or 1
        rerun += last > 1 and run["conclusion"] == "success"
        for attempt in attempts_to_read(run):
            where = f"{run['headBranch']}, run {run['databaseId']}" + (f", attempt {attempt} of {last}" if last > 1 else "")
            for job in failed_jobs(run, attempt):
                if run["conclusion"] == "success":
                    reruns_before_green[job["name"]] += 1
                found = failed_cases(gh("run", "view", "--job", str(job["id"]), "--attempt", str(attempt), "--log"))
                if not found:
                    steps = ", ".join(step["name"] for step in job.get("steps", []) if step["conclusion"] == "failure")
                    other[f"{job['name']}: {steps}"] += 1
                for name, kind in found:
                    cases[name].append(f"{job['name']} {kind} ({where})")

    failures = sum(1 for run in completed if run["conclusion"] == "failure")
    print(f"{len(completed)} completed runs of {arguments.workflow}, {failures} failed, {rerun} passed once run again")
    if truncated:
        window = f"the last {arguments.days:g} days" if arguments.days else "the period"
        print(f"WARNING: the listing hit its limit of {limit} runs: the oldest runs of {window} are missing and the counts below "
              "are incomplete. Raise --limit" + ("." if arguments.days else " or narrow the window with --days."))
    for name, occurrences in sorted(cases.items(), key=lambda item: (-len(item[1]), item[0])):
        print(f"\n{len(occurrences):3d}  {name}")
        for occurrence in occurrences:
            print(f"       {occurrence}")
    print("\nReruns needed before green, per job (failed attempts of runs that finally passed):")
    for name, count in reruns_before_green.most_common():
        print(f"{count:3d}  {name}")
    if not reruns_before_green:
        print("  none")
    if other:
        print("\nFailed jobs without a failed CTest case:")
        for description, count in other.most_common():
            print(f"{count:3d}  {description}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
