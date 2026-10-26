param(
    [string]$InstallRoot = $PSScriptRoot,
    [switch]$Background
)

$ErrorActionPreference = 'Stop'
$runnerDirectory = Join-Path $InstallRoot 'runner'
$policy = Join-Path $InstallRoot 'policy/Authorize-Job.ps1'
if (-not (Test-Path -LiteralPath $policy)) { throw 'Missing local runner authorization policy.' }
if ($Background) {
    $listener = Get-CimInstance Win32_Process -Filter "Name='Runner.Listener.exe'" | Where-Object {
        $_.ExecutablePath -eq (Join-Path $runnerDirectory 'bin/Runner.Listener.exe')
    }
    if ($listener) { throw 'This runner is already running.' }
    $shell = (Get-Command pwsh).Source
    $launcher = Start-Process -FilePath $shell -ArgumentList @(
        '-NoProfile', '-File', ('"' + (Join-Path $InstallRoot 'Start-LocalRunner.ps1') + '"'),
        '-InstallRoot', ('"' + $InstallRoot + '"')
    ) -WorkingDirectory $InstallRoot -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput "$InstallRoot/runner.stdout.log" `
        -RedirectStandardError "$InstallRoot/runner.stderr.log"
    Set-Content -LiteralPath "$InstallRoot/launcher.pid" -Value $launcher.Id
    Write-Host "Runner started in the background (PID $($launcher.Id)). Logs: $InstallRoot/runner.stdout.log"
    return
}
$env:ACTIONS_RUNNER_HOOK_JOB_STARTED = $policy
# Git's bash, rather than the unrelated Windows/WSL bash, is needed by composite actions.
$env:PATH = "C:\Program Files\Git\bin;$env:PATH"
function Set-RunnerGitEnvironment {
    # A daemon started under checkout's temporary Git HOME can hold that directory
    # open on Windows. Override only this runner's processes, preserving user files
    # and any other command-scoped Git configuration inherited by the launcher.
    $entry = if ($env:GIT_CONFIG_COUNT) { [int]$env:GIT_CONFIG_COUNT } else { 0 }
    [Environment]::SetEnvironmentVariable("GIT_CONFIG_KEY_$entry", 'core.fsmonitor', 'Process')
    [Environment]::SetEnvironmentVariable("GIT_CONFIG_VALUE_$entry", 'false', 'Process')
    $env:GIT_CONFIG_COUNT = [string]($entry + 1)
}
Set-RunnerGitEnvironment
Push-Location $runnerDirectory
try {
    # Run the listener directly: cmd's batch wrapper prompts on Ctrl+C, which
    # cannot be answered by a hidden process. Preserve its retry/update behavior.
    do {
        & ./bin/Runner.Listener.exe run
        $runnerExit = $LASTEXITCODE
        if ($runnerExit -eq 2) { Start-Sleep -Seconds 5 }
        if ($runnerExit -in @(3, 4)) {
            for ($attempt = 0; $attempt -lt 30; $attempt++) {
                if (Test-Path -LiteralPath './update.finished') { break }
                Start-Sleep -Seconds 1
            }
        }
    } while ($runnerExit -in @(2, 3, 4))
    if ($runnerExit -ne 0) { throw "Runner stopped with exit code $runnerExit." }
} finally { Pop-Location }
