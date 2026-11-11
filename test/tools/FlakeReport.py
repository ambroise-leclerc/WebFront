#!/usr/bin/env python3
"""Counts the CTest cases that failed in the recent runs of a workflow, per platform job.

Read-only: it lists runs and reads the logs of their failed jobs through the GitHub CLI (`gh`), which must be
authenticated. A case that fails on one platform of a run whose other platforms pass is the usual sign of an
intermittent failure; one that fails everywhere on a branch is more likely a regression of that branch.

    python3 test/tools/FlakeReport.py --limit 60
"""
import argparse
import collections
import json
import re
import subprocess
import sys

# "644/687 Test #615: DeveloperBrowserIntegration-jasmine .....***Failed   18.97 sec", after the log's own prefix.
FAILED_CASE = re.compile(r"\d+/\d+ Test +#\d+: (?P<name>.+?) \.*\*\*\*(?P<kind>Failed|Timeout|Exception[^ ]*|Not Run)")


def failed_cases(log):
    """The (case, kind) pairs of a CTest log; cases that did not run because a dependency failed are left out."""
    found = []
    for line in log.splitlines():
        match = FAILED_CASE.search(line)
        if match and match["kind"] != "Not Run":
            found.append((match["name"].strip(), match["kind"]))
    return found


def gh(*arguments):
    result = subprocess.run(["gh", *arguments], capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode != 0:
        raise RuntimeError(f"gh {' '.join(arguments)}: {result.stderr.strip()}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--workflow", default="BuildAndTest.yml")
    parser.add_argument("--limit", type=int, default=60, help="how many of the latest runs to read")
    parser.add_argument("--branch", help="only the runs of this branch")
    arguments = parser.parse_args()

    listing = ["run", "list", "--workflow", arguments.workflow, "--limit", str(arguments.limit),
               "--json", "databaseId,conclusion,headBranch,event"]
    if arguments.branch:
        listing += ["--branch", arguments.branch]
    runs = json.loads(gh(*listing))
    completed = [run for run in runs if run["conclusion"] in ("success", "failure")]
    cases = collections.defaultdict(list)
    other = collections.Counter()
    for run in completed:
        if run["conclusion"] != "failure":
            continue
        jobs = json.loads(gh("run", "view", str(run["databaseId"]), "--json", "jobs"))["jobs"]
        for job in jobs:
            if job["conclusion"] != "failure":
                continue
            found = failed_cases(gh("run", "view", "--job", str(job["databaseId"]), "--log-failed"))
            if not found:
                steps = ", ".join(step["name"] for step in job["steps"] if step["conclusion"] == "failure")
                other[f"{job['name']}: {steps}"] += 1
            for name, kind in found:
                cases[name].append(f"{job['name']} {kind} ({run['headBranch']}, run {run['databaseId']})")

    failures = sum(1 for run in completed if run["conclusion"] == "failure")
    print(f"{len(completed)} completed runs of {arguments.workflow}, {failures} failed")
    for name, occurrences in sorted(cases.items(), key=lambda item: (-len(item[1]), item[0])):
        print(f"\n{len(occurrences):3d}  {name}")
        for occurrence in occurrences:
            print(f"       {occurrence}")
    if other:
        print("\nFailed jobs without a failed CTest case:")
        for description, count in other.most_common():
            print(f"{count:3d}  {description}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
