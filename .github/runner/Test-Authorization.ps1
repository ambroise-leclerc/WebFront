$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Authorize-Job.ps1" -DefineOnly

function New-Fixture {
    @{
        Context = @{
            Repository = 'ambroise-leclerc/WebFront'; Actor = 'ambroise-leclerc'
            ActorId = '6860842'; TriggeringActor = 'ambroise-leclerc'
            EventName = 'pull_request'; Ref = 'refs/pull/1/merge'
        }
        Event = @{
            sender = @{ id = 6860842 }
            repository = @{ full_name = 'ambroise-leclerc/WebFront' }
            pull_request = @{
                user = @{ id = 6860842 }
                head = @{ repo = @{ full_name = 'ambroise-leclerc/WebFront' } }
                base = @{ repo = @{ full_name = 'ambroise-leclerc/WebFront' } }
            }
        }
    }
}

$cases = @(
    @{ Name = 'owner PR'; Allow = $true; Change = {} },
    @{ Name = 'manual owner run'; Allow = $true; Change = { param($f) $f.Context.EventName = 'workflow_dispatch' } },
    @{ Name = 'owner push'; Allow = $true; Change = { param($f) $f.Context.EventName = 'push'; $f.Event.ref = $f.Context.Ref } },
    @{ Name = 'other actor'; Change = { param($f) $f.Context.Actor = 'other' } },
    @{ Name = 'other actor ID'; Change = { param($f) $f.Context.ActorId = '1' } },
    @{ Name = 'other rerun initiator'; Change = { param($f) $f.Context.TriggeringActor = 'other' } },
    @{ Name = 'other sender'; Change = { param($f) $f.Event.sender.id = 1 } },
    @{ Name = 'other PR author'; Change = { param($f) $f.Event.pull_request.user.id = 1 } },
    @{ Name = 'fork'; Change = { param($f) $f.Event.pull_request.head.repo.full_name = 'other/WebFront' } },
    @{ Name = 'other repository'; Change = { param($f) $f.Context.Repository = 'other/WebFront' } },
    @{ Name = 'missing actor'; Change = { param($f) $f.Context.Actor = $null } },
    @{ Name = 'missing payload'; Change = { param($f) $f.Event = @{} } },
    @{ Name = 'pull_request_target'; Change = { param($f) $f.Context.EventName = 'pull_request_target' } },
    @{ Name = 'schedule'; Change = { param($f) $f.Context.EventName = 'schedule' } },
    @{ Name = 'deleted push'; Change = { param($f) $f.Context.EventName = 'push'; $f.Event.ref = $f.Context.Ref; $f.Event.deleted = $true } },
    @{ Name = 'wrong push ref'; Change = { param($f) $f.Context.EventName = 'push'; $f.Event.ref = 'refs/heads/other' } }
)
foreach ($case in $cases) {
    $fixture = New-Fixture
    & $case.Change $fixture
    $allowed = $true
    try { Assert-AuthorizedRunnerJob $fixture.Context $fixture.Event } catch { $allowed = $false }
    if ($allowed -ne [bool]$case.Allow) { throw "Authorization regression: $($case.Name)" }
    Write-Host "PASS: $($case.Name)"
}
