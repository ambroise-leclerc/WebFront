# Jetson Orin hardware runner (#529)

The `Jetson Orin` workflow validates trusted code on the maintainer's Orin Nano
8 GB workstation, with the installed Tegra driver. It is S2 evidence, never a
required PR check. An offline board can leave the job queued; it must not block
a merge. Do not add it to branch protection. Three consecutive green weekly
runs and observation that the workstation remains usable are the acceptance
criteria; a manual pass does not establish those criteria.

## Trust boundary

The dedicated workflow accepts only Monday 01:17 UTC on the default branch (`develop`), pushes to `master`, and
manual dispatch by `ambroise-leclerc` on a branch of this repository. There is no
PR, PR-target, reusable-workflow, or workflow-run trigger. Manual dispatch checks
the selected branch, so review its contents before launching it. No user input
is interpolated into shell code, checkout persists no credentials, and the job
has only `contents: read` permission. The board has no deployment secrets.

For pre-merge validation, the existing `BuildAndTest.yml` accepts
`jetson_orin=true` on maintainer dispatch only. Its `jetson-validation` job has
the same resource limits and runner labels, and cannot run on PR events. This
entry point is necessary because GitHub cannot dispatch a new workflow before
it exists on the default branch. Dispatch it with:

```sh
gh workflow run BuildAndTest.yml --ref <trusted-branch> -f jetson_orin=true
```

`HardwareWorkflows` in the fast CTest tier fixes the hardware workflow's literal
control plane and the manual validation job, and rejects hardware labels elsewhere. Its mutation tests
cover fork PR and PR-target triggers, additional jobs, weakened ref/actor checks
and write permissions. It intentionally fails closed on control-plane edits.
This is regression coverage, not an access-control boundary: a PR author can
also edit a test. Review workflow edits before approving any external run.

Before connecting the runner, require approval for **all external contributors**
in Settings → Actions → General. Inspect without displaying credentials:

```sh
gh api repos/ambroise-leclerc/WebFront/actions/permissions/fork-pr-contributor-approval
```

The expected `approval_policy` is `all_external_contributors`. The earlier
`first_time_contributors` value does not cover returning external contributors.
Never approve a fork workflow that selects this runner or changes these guards.
GitHub recommends private repositories for self-hosted runners; a dedicated
unprivileged account limits accidental access but does not sandbox hostile code.
See [GitHub's runner guidance](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/add-runners)
and [secure use](https://docs.github.com/en/actions/reference/security/secure-use).

## Provisioning

Follow `JETSON.md` for the installed L4T, GCC, CMake and WebKitGTK prerequisites.
The service also needs Ninja, ccache, Xvfb, ionice, Python 3, Git, CUDA/nvcc and
access to the Tegra GPU (`video` and `render` groups). Install these as an
administrator before the job; the workflow never uses sudo or installs packages.
Use a dedicated `webfront-runner` system account, home `/var/lib/webfront-runner`,
runner `/var/lib/webfront-runner/runner`, work `_work` within that directory, and
cache `/var/cache/webfront-runner`, all owned by that account. Do not add it to
`sudo`, `docker`, or the maintainer's groups, or give it desktop credentials.

In Settings → Actions → Runners → New self-hosted runner, select Linux ARM64.
Download the offered `actions/runner` release, verify the supplied SHA-256 before
extracting it as the service account, then configure from the runner directory:

```sh
./config.sh --url https://github.com/ambroise-leclerc/WebFront \
  --name webfront-jetson-orin --labels linux,arm64,jetson-orin \
  --work _work --unattended --token "$REGISTRATION_TOKEN"
```

Obtain the short-lived registration token just before configuring and never save
it to Git, shell history, logs or the service environment. Use the runner's
`svc.sh install webfront-runner` as administrator. Add a systemd service drop-in:

```ini
[Service]
Environment=WEBFRONT_JETSON_CACHE=/var/cache/webfront-runner
Environment=CC=gcc-13
Environment=CXX=g++-13
Environment=PATH=/usr/local/cuda/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin
Nice=15
IOSchedulingClass=idle
CPUQuota=150%
MemoryHigh=4608M
MemoryMax=5G
MemorySwapMax=512M
ProtectHome=true
ProtectSystem=full
PrivateTmp=true
NoNewPrivileges=true
UMask=0077
```

The provisioned board uses CMake 4.4.4 under `/opt/webfront-runner-tools/cmake/data/bin`,
prepended to the service PATH and to the runner's `.path` file (which its startup
script reads in preference to the service environment). CMake must be available in this PATH (minimum version in `CMakeLists.txt`). If
nvcc needs a different host compiler, supply `CUDAHOSTCXX`; do not disable its
version check. Then `systemctl daemon-reload` and start the runner service.
The soft threshold is 4.5 GiB: two GCC units can exceed 4 GiB together.
These limits leave resources for the desktop; memory pressure can still make a
build fail, which is preferable to making the workstation unusable. The job
never changes the power mode; `nvpmodel -q` is read-only.

## Work and storage budget

A single concurrency group serializes all board work; manual dispatch is an
explicit exception to the nocturnal slot. The timeout is 180 minutes, allowing
cold builds on a throttled 8 GB board. Every build and CTest uses `--parallel 2`,
under `nice` and idle I/O priority. A Ninja compile pool admits one compiler at
a time: two GCC unity units together exceeded 4.5 GiB and caused sustained page
reclaim on the workstation even within the service's memory cap. Linking and
CTest can still use two workers. The script clears inherited display and
software-renderer overrides; browser tests use the suite's Xvfb wrapper. They do
not open windows on the maintainer's session. Xvfb does not qualify physical
presentation or prove WebGL acceleration: the manual checks in `JETSON.md` still
apply.

The job builds CEF-off with WebKitGTK disabled and runs the complete CTest suite,
then builds WebKitGTK 6.0 and runs `web-integration` on the real Tegra stack.
CUDA is required (`WEBFRONT_HOST_LOOP_CUDA=ON`, Orin SM 87); its CPU/GPU image
comparison is also run directly so a missing GPU produces exit 4 instead of a
successful CTest skip. CEF remains optional and is covered by the separate hosted
`cef-arm64` job; it is not added to the workstation's cold-build budget.

ccache persists outside checkout with a 6 GiB limit. CPM sources persist in a
separate directory, cleared at the beginning of a job if larger than 8 GiB.
CUDA's disk cache is capped at 256 MiB. Only the two fixed `build-jetson-*`
directories are removed before and after a job, including failed runs. A forced
service stop can leave a build until the next run; no other workspace or user
build is removed. Dependencies downloaded during configure can temporarily
exceed the CPM threshold; this is a high-water cleanup, not a filesystem quota.
The checkout stays as one working copy. Artifacts retain JUnit and CTest logs for
14 days, and the job summary records L4T/JetPack package versions, compiler,
CMake, CUDA, WebKitGTK and the current power mode.

## Recovery

1. Stop the service before maintenance. Preserve failed-job artifacts and the
   runner's `_diag` logs; remove secrets before sharing diagnostics.
2. For a runner update, let its built-in updater run, or stop the service and
   replace the installation with a checksum-verified ARM64 release from
   `actions/runner`. Keep the account, labels, work path and resource drop-in.
3. For lost or exposed credentials, remove the runner in GitHub's Runners UI,
   stop its service, remove its local registration with a fresh removal token,
   and re-register using a fresh short-lived registration token. A registration
   token is not a permanent PAT; never keep a PAT on the board.
4. If storage is exhausted, stop the service and remove only this account's two
   job builds and dedicated caches. Restart with cold caches; do not clean the
   maintainer's existing builds or change the board's power mode.
5. After a suspected compromise, revoke/remove the runner registration first.
   Reflash with the chosen NVIDIA image, update the OS, follow `JETSON.md`,
   recreate the unprivileged account and verified runner installation, and
   restore no executable cache from the old image. Recheck external-approval
   policy before starting the service.
6. Dispatch a trusted branch manually, inspect CPU/GPU regression and browser
   results and workstation responsiveness, then record the next three weekly
   run links. Do not claim the weekly acceptance criterion from manual runs.
