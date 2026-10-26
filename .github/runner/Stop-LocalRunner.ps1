param([string]$InstallRoot = $PSScriptRoot)

$ErrorActionPreference = 'Stop'
$launcherId = [int](Get-Content -LiteralPath "$InstallRoot/launcher.pid")
$launcher = Get-CimInstance Win32_Process -Filter "ProcessId=$launcherId"
if (-not $launcher) { Write-Host 'Background runner is already stopped.'; return }
if ($launcher.Name -ne 'pwsh.exe' -or
    -not $launcher.CommandLine.Replace('/', '\').Contains(
        (Join-Path $InstallRoot 'Start-LocalRunner.ps1').Replace('/', '\'))) {
    throw 'Stored PID does not identify this runner launcher; no process was stopped.'
}
$settings = Get-Content -LiteralPath "$InstallRoot/runner/.runner" -Raw | ConvertFrom-Json
$runner = & gh api "repos/ambroise-leclerc/WebFront/actions/runners/$($settings.agentId)" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Cannot check runner activity; no process was stopped.' }
if ($runner.busy) { throw 'Runner is busy. Wait for the current job before stopping it.' }
# Send Ctrl+C from a separate process so the listener can release its GitHub session.
# The helper attaches only to the console of the verified launcher, never this shell.
$signalScript = @'
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class RunnerConsole {
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool FreeConsole();
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool AttachConsole(uint processId);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool GenerateConsoleCtrlEvent(uint signal, uint group);
}
"@
[RunnerConsole]::FreeConsole() | Out-Null
if (-not [RunnerConsole]::AttachConsole(LAUNCHER_ID)) { exit 1 }
[RunnerConsole]::SetConsoleCtrlHandler([IntPtr]::Zero, $true) | Out-Null
if (-not [RunnerConsole]::GenerateConsoleCtrlEvent(0, 0)) { exit 2 }
Start-Sleep -Milliseconds 500
'@
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes(
    $signalScript.Replace('LAUNCHER_ID', [string]$launcherId)))
$helper = Start-Process -FilePath (Get-Command pwsh).Source -WindowStyle Hidden -Wait -PassThru `
    -ArgumentList @('-NoProfile', '-EncodedCommand', $encoded)
if ($helper.ExitCode -ne 0) { throw 'Cannot signal the runner console; no process was forcibly terminated.' }
$remaining = Get-Process -Id $launcherId -ErrorAction SilentlyContinue
if ($remaining) { $remaining | Wait-Process -Timeout 30 -ErrorAction Stop }
Write-Host 'Background runner stopped.'
