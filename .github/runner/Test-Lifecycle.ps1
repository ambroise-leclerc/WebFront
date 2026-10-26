$ErrorActionPreference = 'Stop'
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('webfront-runner-guard-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'policy') -Force | Out-Null
Set-Content -LiteralPath (Join-Path $fixtureRoot 'policy/Authorize-Job.ps1') -Value '# Fixture only'

function Get-CimInstance {
    param($ClassName, $Filter)
    # CIM returns native Windows separators even when the installer used slashes.
    [pscustomobject]@{ ExecutablePath = "$fixtureRoot\runner\bin\Runner.Listener.exe".Replace('/', '\') }
}
function Start-Process {
    $script:processStarted = $true
    throw 'A second launcher must not be started.'
}

try {
    foreach ($root in @($fixtureRoot, $fixtureRoot.Replace('\', '/'), "$fixtureRoot/")) {
        $script:processStarted = $false
        $denied = $false
        try { & "$PSScriptRoot/Start-LocalRunner.ps1" -InstallRoot $root -Background }
        catch { $denied = $_.Exception.Message -eq 'This runner is already running.' }
        if (-not $denied -or $script:processStarted -or (Test-Path -LiteralPath "$fixtureRoot/launcher.pid")) {
            throw "Duplicate runner guard failed for $root"
        }
        Write-Host "PASS: duplicate background start refused for $root"
    }
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        "$PSScriptRoot/Start-LocalRunner.ps1", [ref]$tokens, [ref]$errors)
    $configure = $ast.Find({ param($syntaxElement)
        $syntaxElement -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $syntaxElement.Name -eq 'Set-RunnerGitEnvironment'
    }, $true)
    if ($configure) { . ([scriptblock]::Create($configure.Extent.Text)) }
    else { function Set-RunnerGitEnvironment { } }
    $variables = @('GIT_CONFIG_GLOBAL', 'GIT_CONFIG_COUNT', 'GIT_CONFIG_KEY_0', 'GIT_CONFIG_VALUE_0',
        'GIT_CONFIG_KEY_1', 'GIT_CONFIG_VALUE_1')
    $saved = @{}
    foreach ($name in $variables) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
    try {
        $config = Join-Path $fixtureRoot 'gitconfig'
        [IO.File]::WriteAllText($config, "[core]`n    fsmonitor = true`n")
        $env:GIT_CONFIG_GLOBAL = $config
        foreach ($inheritedCount in @(0, 1)) {
            $env:GIT_CONFIG_COUNT = [string]$inheritedCount
            $env:GIT_CONFIG_KEY_0 = 'webfront.test'
            $env:GIT_CONFIG_VALUE_0 = 'preserved'
            if ((& git config --get core.fsmonitor) -ne 'true') { throw 'Invalid Git fixture' }
            Set-RunnerGitEnvironment
            if ((& git config --get core.fsmonitor) -ne 'false') { throw 'Inherited fsmonitor is still enabled' }
            if ($inheritedCount -eq 1 -and (& git config --get webfront.test) -ne 'preserved') {
                throw 'Existing Git environment configuration was lost.'
            }
            if ([IO.File]::ReadAllText($config) -notmatch 'fsmonitor = true') {
                throw 'The inherited Git configuration file was modified.'
            }
            Write-Host "PASS: fsmonitor disabled without modifying inherited configuration ($inheritedCount entries)"
        }
    } finally {
        foreach ($name in $variables) {
            if ($null -eq $saved[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
            else { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
        }
    }
} finally {
    # Only the unique fixture created by this test is removed.
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($fixtureRoot)).TrimEnd('\', '/') -ne
        [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')) {
        throw 'Refusing to remove a fixture outside the temporary directory.'
    }
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
}
