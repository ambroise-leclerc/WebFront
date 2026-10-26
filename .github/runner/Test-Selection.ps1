$ErrorActionPreference = 'Stop'
$workflow = Get-Content -LiteralPath "$PSScriptRoot/../workflows/BuildAndTest.yml" -Raw
$match = [regex]::Match($workflow, '(?s)          function Select-WindowsRunner \{.*?(?=          # End of selection function)')
if (-not $match.Success) { throw 'Cannot locate the selection function in the workflow.' }
. ([scriptblock]::Create(($match.Value -replace '(?m)^          ', '')))

function Invoke-RestMethod {
    param($TimeoutSec, $Headers, $Uri)
    $script:queried = $true
    if ($script:apiError) { throw 'Simulated unavailable API' }
    @{ runners = $script:runners }
}

$cases = @(
    @{ Name = 'owner manual run'; Local = $true; Change = {} },
    @{ Name = 'owner PR'; Local = $true; Change = { $context.EventName = 'pull_request' } },
    @{ Name = 'owner push'; Local = $true; Change = { $context.EventName = 'push' } },
    @{ Name = 'other actor'; NoQuery = $true; Change = { $context.Actor = 'other' } },
    @{ Name = 'other actor ID'; NoQuery = $true; Change = { $context.ActorId = '1' } },
    @{ Name = 'other sender'; NoQuery = $true; Change = { $event.sender.id = 1 } },
    @{ Name = 'other rerun initiator'; NoQuery = $true; Change = { $context.TriggeringActor = 'other' } },
    @{ Name = 'other PR author'; NoQuery = $true; Change = { $event.pull_request.user.id = 1 } },
    @{ Name = 'missing PR'; NoQuery = $true; Change = { $context.EventName = 'pull_request'; $event.pull_request = $null } },
    @{ Name = 'fork'; NoQuery = $true; Change = { $event.pull_request.head.repo.full_name = 'other/WebFront' } },
    @{ Name = 'other repository'; NoQuery = $true; Change = { $context.Repository = 'other/WebFront' } },
    @{ Name = 'schedule'; NoQuery = $true; Change = { $context.EventName = 'schedule' } },
    @{ Name = 'pull_request_target'; NoQuery = $true; Change = { $context.EventName = 'pull_request_target' } },
    @{ Name = 'missing token'; NoQuery = $true; Change = { $script:token = '' } },
    @{ Name = 'offline'; Change = { $script:runners[0].status = 'offline' } },
    @{ Name = 'busy'; Change = { $script:runners[0].busy = $true } },
    @{ Name = 'wrong label'; Change = { $script:runners[0].labels = @(@{ name = 'other' }) } },
    @{ Name = 'missing unique label'; Change = { $script:runners[0].labels = @(@{ name = 'webfront-local-windows' }) } },
    @{ Name = 'no runner'; Change = { $script:runners = @() } },
    @{ Name = 'API unavailable'; Change = { $script:apiError = $true } }
)
foreach ($case in $cases) {
    $context = @{
        Repository = 'ambroise-leclerc/WebFront'; Actor = 'ambroise-leclerc'; ActorId = '6860842'
        TriggeringActor = 'ambroise-leclerc'; EventName = 'workflow_dispatch'
    }
    $event = @{
        sender = @{ id = 6860842 }
        pull_request = @{ user = @{ id = 6860842 }; head = @{ repo = @{ full_name = 'ambroise-leclerc/WebFront' } } }
    }
    $script:runners = @(@{ name = 'webfront-test'; status = 'online'; busy = $false
        labels = @(@{ name = 'webfront-local-windows' }, @{ name = 'webfront-test' }) })
    $script:token = 'test-credential'
    $script:queried = $false
    $script:apiError = $false
    . $case.Change
    $labels = Select-WindowsRunner $context $event $script:token
    $expected = if ($case.Local) { 'webfront-local-windows,webfront-test' } else { 'windows-2025' }
    if (($labels -join ',') -cne $expected -or ($case.NoQuery -and $script:queried)) {
        throw "Runner selection regression: $($case.Name)"
    }
    Write-Host "PASS: $($case.Name)"
}
