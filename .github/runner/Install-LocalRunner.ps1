param(
    [string]$InstallRoot = "$env:LOCALAPPDATA/WebFrontRunner"
)

$ErrorActionPreference = 'Stop'
$repository = 'ambroise-leclerc/WebFront'
$runnerDirectory = Join-Path $InstallRoot 'runner'
$policyDirectory = Join-Path $InstallRoot 'policy'
if (Test-Path -LiteralPath $runnerDirectory) {
    throw "Installation directory already exists: $runnerDirectory. It will not be overwritten."
}
$identity = (& gh api user | ConvertFrom-Json)
if ($LASTEXITCODE -ne 0 -or $identity.id -ne 6860842) { throw 'Authenticate gh as ambroise-leclerc first.' }
& "$PSScriptRoot/Test-Authorization.ps1"
if ($LASTEXITCODE -ne 0) { throw 'Authorization tests failed.' }

$release = & gh api repos/actions/runner/releases/latest | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Cannot read runner release.' }
$asset = $release.assets | Where-Object name -Match '^actions-runner-win-x64-[0-9.]+\.zip$'
if (@($asset).Count -ne 1 -or $asset.digest -notmatch '^sha256:([a-f0-9]{64})$') {
    throw 'Runner release must have exactly one Windows x64 archive with a SHA-256 digest.'
}
$expected = $Matches[1]
New-Item -ItemType Directory -Path $runnerDirectory, $policyDirectory -Force | Out-Null
$archive = Join-Path $InstallRoot $asset.name
Invoke-WebRequest $asset.browser_download_url -OutFile $archive
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ine $expected) {
    throw 'Runner archive SHA-256 mismatch.'
}
Expand-Archive -LiteralPath $archive -DestinationPath $runnerDirectory
Copy-Item -LiteralPath "$PSScriptRoot/Authorize-Job.ps1" -Destination $policyDirectory
# Keep the policy outside both the runner distribution and the checked-out workspace.
Set-Content -LiteralPath "$runnerDirectory/.env" -Encoding utf8 -Value (
    'ACTIONS_RUNNER_HOOK_JOB_STARTED=' + (Join-Path $policyDirectory 'Authorize-Job.ps1'))
$registration = & gh api --method POST "repos/$repository/actions/runners/registration-token" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or -not $registration.token) { throw 'Cannot get runner registration token.' }
Push-Location $runnerDirectory
try {
    & ./config.cmd --unattended --url "https://github.com/$repository" --token $registration.token `
        --name "webfront-$env:COMPUTERNAME" --work '_work' --no-default-labels `
        --labels "webfront-local-windows,webfront-$env:COMPUTERNAME"
    if ($LASTEXITCODE -ne 0) { throw 'Runner registration failed.' }
} finally {
    $registration = $null
    Pop-Location
}
Copy-Item -LiteralPath "$PSScriptRoot/Start-LocalRunner.ps1" -Destination $InstallRoot
Copy-Item -LiteralPath "$PSScriptRoot/Stop-LocalRunner.ps1" -Destination $InstallRoot
Write-Host "Installed at $InstallRoot. Start with: & '$InstallRoot/Start-LocalRunner.ps1'"
