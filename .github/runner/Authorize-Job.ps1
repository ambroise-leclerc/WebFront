param([switch]$DefineOnly)

$ErrorActionPreference = 'Stop'

function Assert-AuthorizedRunnerJob {
    param([hashtable]$Context, $Event)

    $owner = 'ambroise-leclerc'
    $ownerId = '6860842'
    $repository = 'ambroise-leclerc/WebFront'
    if ($Context.Repository -cne $repository -or
        $Context.Actor -cne $owner -or $Context.ActorId -cne $ownerId -or
        $Context.TriggeringActor -cne $owner -or
        [string]$Event.sender.id -cne $ownerId -or
        $Event.repository.full_name -cne $repository) {
        throw 'Local runner denied: repository, actor, sender and rerun initiator must match the owner.'
    }
    switch -CaseSensitive ($Context.EventName) {
        'push' {
            if ($Event.deleted -or $Event.ref -cne $Context.Ref) {
                throw 'Local runner denied: invalid push.'
            }
        }
        'workflow_dispatch' { }
        'pull_request' {
            if ([string]$Event.pull_request.user.id -cne $ownerId -or
                $Event.pull_request.head.repo.full_name -cne $repository -or
                $Event.pull_request.base.repo.full_name -cne $repository) {
                throw 'Local runner denied: only owner pull requests from this repository are allowed.'
            }
        }
        default { throw 'Local runner denied: unsupported event (including fork and scheduled events).' }
    }
}

if (-not $DefineOnly) {
    try {
        if (-not $env:GITHUB_EVENT_PATH) { throw 'Missing GitHub event payload.' }
        $event = Get-Content -LiteralPath $env:GITHUB_EVENT_PATH -Raw | ConvertFrom-Json
        Assert-AuthorizedRunnerJob @{
            Repository = $env:GITHUB_REPOSITORY
            Actor = $env:GITHUB_ACTOR
            ActorId = $env:GITHUB_ACTOR_ID
            TriggeringActor = $env:GITHUB_TRIGGERING_ACTOR
            EventName = $env:GITHUB_EVENT_NAME
            Ref = $env:GITHUB_REF
        } $event
        Write-Host 'Local runner authorization passed.'
    } catch {
        Write-Error $_ -ErrorAction Continue
        exit 1
    }
}
