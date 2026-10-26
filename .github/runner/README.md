# Optional local Windows runner

The Windows desktop lane of `BuildAndTest.yml` can use a local runner. Other lanes
continue to use GitHub-hosted machines. Installation requires PowerShell 7, Git for
Windows, the MSVC C++ build tools, Ninja, and `gh` authenticated as
`ambroise-leclerc`. Run from the repository:

The CI resource-profiling tools also require Python 3.10 or newer through the
Windows `py -3` launcher. Linux and macOS continue to use `python3`.

```powershell
./.github/runner/Install-LocalRunner.ps1
& "$env:LOCALAPPDATA/WebFrontRunner/Start-LocalRunner.ps1"
```

The installer verifies the official runner archive's SHA-256, registers it for
this repository only, and copies the authorization hook outside the checkout and
runner distribution. It refuses to overwrite an existing installation. No
Windows service or automatic startup is installed. Ctrl+C stops the foreground
runner; restarting the same script brings it online again. Runner updates are
managed by GitHub's runner. To update the policy after reviewing a change, stop
the runner and copy `Authorize-Job.ps1` to
`$env:LOCALAPPDATA/WebFrontRunner/policy/Authorize-Job.ps1`, then restart it.

For a hidden background process, use `Start-LocalRunner.ps1 -Background`; logs are
written to `runner.stdout.log` and `runner.stderr.log` beside that script. Stop it
with `Stop-LocalRunner.ps1` in the installation directory. This checks that the
stored PID belongs to this launcher and refuses to stop it while a job is busy.
It requires `gh` authentication to check activity. No startup task is created.
The launcher disables Git's inherited filesystem monitor for its child processes:
a monitor daemon can keep checkout's temporary configuration directory open on
Windows. This uses command-scoped environment settings and preserves personal
Git configuration files and other inherited Git environment entries.

## Availability credential

Create a fine-grained personal access token limited to `ambroise-leclerc/WebFront`,
with repository **Administration: read-only**, and save it as the Actions secret
`LOCAL_RUNNER_STATUS_TOKEN`. Set an expiry and rotate it before expiry. Listing
repository runners requires this permission; `GITHUB_TOKEN` cannot grant it.
Do not use your general-purpose `gh` token as this secret.

The hosted selection job checks the live runner API, without checking out PR code
first. Only an **online and idle** matching runner is selected, using its unique
name label. Missing/expired credentials, API errors, offline/busy runners and
untrusted events all select `windows-2025`. No probe credential is passed to the
local runner. A runner can still disconnect or become busy between the probe and
assignment: GitHub provides no atomic reservation or automatic hosted fallback
once a job has been assigned. Cancel and rerun that workflow if this happens.

## Authorization and limits

Both selection and the installed job-start hook require the GitHub actor and
rerun initiator to be `ambroise-leclerc`, with actor/sender ID `6860842`, for this
repository. PRs also require this author and a head branch in this repository;
fork PRs are refused even when the owner reruns them. Only `push`, `pull_request`
and `workflow_dispatch` events are accepted. Scheduled jobs remain hosted.

The runner has no default labels, so generic `self-hosted` jobs cannot select it.
The local hook fails closed before workflow steps, including checkout, even if a
workflow directly requests its custom label. A refused job can be assigned to the
runner but its steps will not execute. The runner itself still prepares the job
and downloads actions before the hook. This is an author policy, not a sandbox:
accepted jobs run as the local Windows user and must be reviewed, including their
dependencies. Do not approve untrusted code by triggering it under your account.
The destructive `remove-node` action is skipped on this persistent machine.

Regression checks:

```powershell
./.github/runner/Test-Authorization.ps1
./.github/runner/Test-Selection.ps1
./.github/runner/Test-Lifecycle.ps1 # Windows only
```

References: [runner API permissions](https://docs.github.com/en/rest/actions/self-hosted-runners#list-self-hosted-runners-for-a-repository),
[job-start hooks](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/run-scripts).
