# Windows qualification of PR #368

## Scope and host

Qualification started on 2026-10-25 from `1151dc4f798d0c2b2e8adf0065e6a65d36d2fcc9`, the head of PR #368.
The host is Curmudgeon, running Windows 11 Pro for Workstations, version `10.0.22631`.
The development account has a non-elevated token but belongs to Administrators. It is **not** the dedicated standard
account required by [the local CI contract](LOCAL_CI.md#native-windows-execution).

Observed tools: MSVC `19.44.35229` for x64, Ninja, Python `3.10.11`, and WebView2 Evergreen `155.0.4283.60` / `155.0.4283.62`.
The installed CMake was `4.3.4` and sccache was absent from PATH. Portable CMake `4.4.4` and sccache `0.18.0` were prepared
under the ignored `build-local-ci/windows-qualification-tools/` directory. The CMake ZIP was checked against Kitware's
release SHA-256 list; the sccache ZIP against the WinGet manifest SHA-256
`8965c74d5e8a225244f741e18ad2f3f504f48228dc1bac948fc22761a348363d`.
These local tools do not provision the dedicated account's PATH.

## Findings

The real session worker passes its desktop check, then fails before configure because
`WebFrontLocalCI-BlockPublic` is absent. It writes `{"code": 1}` to its result file. All three firewall profiles are enabled.
The SSH server, `CurmudgeonWindows` client alias, dedicated account and offline kit had not been provisioned.
No account, SSH service or firewall policy was changed by this qualification.

This failure exposed unreadable localized PowerShell diagnostics: captured output used Python's locale encoding while
PowerShell emitted different bytes, and uncaught errors were serialized as CLIXML. The Windows probe helper now sets
UTF-8 explicitly on both sides, suppresses progress records, and formats caught errors as text while retaining exit code 1.
A real PowerShell regression first failed on accented/CJK output, then passed after the fix. It also checks that errors
preserve Unicode and are not CLIXML. This concerns the native worker's probes; binary artifact transport is unchanged.

## Validation

The initial CEF-off/WebView2 Release build with the installed CMake 4.3.4 passes **342/342 CTest cases**, including
73 cases labelled `web-integration` (477.26 seconds total). This includes Jasmine, native lifecycle, transport benchmark,
developer environment, independent consumer configuration and the local CI regressions.
The diagnostic fix separately passes **29/29 Windows worker regressions**, including real PowerShell and Job Object tests.
These run in the development session with public networking available; they do not qualify the dedicated SSH account.

A second clean Release tree uses portable **CMake 4.4.4**, **sccache 0.18.0**, a new cache, and the extracted verified kit.
Its configuration uses the kit's preload, bootstrap and registry directories, `FETCHCONTENT_FULLY_DISCONNECTED=ON`,
`CPM_SOURCE_CACHE` in the environment and `CMAKE_CXX_COMPILER_LAUNCHER=sccache`, with module scanning disabled and four jobs.
Configuration passes in 10.6 seconds, compilation in 459.0 seconds (158 sccache misses, no failures), and **342/342 CTest
cases pass**, including the same 73 browser integration cases, in 341.33 seconds. JUnit records zero failures and zero skips.
The macOS follow-up `8e32530` is integrated, and the updated Windows worker regression runs in this suite.
Logs and JUnit remain in `build-local-ci/windows-qualification-{configure,build,test}.log` and
`build-368-windows-offline-kit/test-results.xml`. Public egress was not blocked for these direct development-session runs.
Their elapsed times are supplementary observations, not the dedicated-account or same-commit GitHub comparison.
After `cmake --build build-368-windows-offline-kit --target clean`, rebuilding the same tree with the retained cache
succeeds in **22.5 seconds**, with **155/155 sccache hits**, zero misses and zero compilation failures. This demonstrates
local persistent-cache reuse; it does not exercise a second SSH run or a new remote checkout.
The warm-build log is `build-local-ci/windows-qualification-warm-build.log`.

## Prepared offline kit

The local archive `build-local-ci/windows-1151dc4.tgz` contains 1,298 inventoried files (32,723,039 compressed bytes):

```text
SHA-256  520f1ec91ec25da36fc8efb1c8ddc25aeab8d90a82f5a0d38bb8b84d653e039e
```

stdnet, stdexec and Catch2 were exported with `git archive` at their pinned revisions, without Git metadata.
The WebView2 SDK archive was checked against the SHA-256 in `WebView2Support.cmake`. The bootstrap and all 14 registry
archives were checked against their pinned SHA-512 values. CPM `0.43.2` was compared with its upstream release module.
The portable preload uses paths relative to itself. `provision.json` records source URLs, revisions and integrity.
Packing, Windows-safe extraction and verification of the extracted inventory all succeeded.

## Owner preparation commands

Apply the following only on a **dedicated CI host or VM**. The public egress block affects every user and program on that
host, including multicast/broadcast services as described in `LOCAL_CI.md`. Finish downloads and verify private-LAN SSH
before applying it. The commands are an owner procedure, not actions performed by the worker.

In an administrator PowerShell:

```powershell
$password = Read-Host 'Password for webfront-ci' -AsSecureString
New-LocalUser -Name webfront-ci -Password $password -Description 'WebFront qualification'
Get-LocalGroup -SID S-1-5-32-545 | Add-LocalGroupMember -Member webfront-ci
Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
Set-Service sshd -StartupType Automatic
Start-Service sshd
```

Log on graphically as `.\webfront-ci` to create its profile. In that account, before blocking public egress:

```powershell
winget install --exact --id Git.Git --accept-source-agreements --accept-package-agreements
winget install --exact --id Python.Python.3.13 --accept-source-agreements --accept-package-agreements
winget install --exact --id Ninja-build.Ninja --accept-source-agreements --accept-package-agreements
winget install --exact --id Mozilla.sccache --accept-source-agreements --accept-package-agreements
winget install --exact --id Kitware.CMake --version 4.4.4 --accept-source-agreements --accept-package-agreements
```

The host already has MSVC C++ x64 tools, a Windows SDK, `vswhere` and WebView2 Evergreen installed. On a new host, install
them separately. Reopen the terminal and verify `git --version`, `python.exe --version`, `ninja --version`,
`sccache --version` and `cmake --version`. `python.exe` must resolve to a real interpreter, not the Microsoft Store alias.
Also verify PATH through SSH; an interactive terminal alone does not demonstrate the SSH service sees these tools.

On the orchestrator, create a key and display **only the public key**:

```powershell
ssh-keygen -t ed25519 -f "$env:USERPROFILE\.ssh\webfront-ci"
Get-Content "$env:USERPROFILE\.ssh\webfront-ci.pub"
```

In the `webfront-ci` session, paste that public-key line into `authorized_keys`:

```powershell
New-Item -ItemType Directory -Force "$env:USERPROFILE\.ssh"
notepad "$env:USERPROFILE\.ssh\authorized_keys"
```

Configure the orchestrator's `%USERPROFILE%\.ssh\config`, replacing the address with the host's private LAN address:

```sshconfig
Host CurmudgeonWindows
    HostName <private-LAN-address>
    User webfront-ci
    IdentityFile ~/.ssh/webfront-ci
    IdentitiesOnly yes
```

Verify the server fingerprint on the first connection and test key authentication:

```powershell
ssh CurmudgeonWindows whoami
ssh -o BatchMode=yes CurmudgeonWindows python.exe --version
```

An encrypted private key must already be loaded into the orchestrator's SSH agent for the non-interactive run.

To require key-only access for this account, append this block to the host's
`C:\ProgramData\ssh\sshd_config` in an administrator editor:

```sshconfig
Match User webfront-ci
    AuthenticationMethods publickey
    PasswordAuthentication no
```

Validate the configuration before restarting the service, then repeat the connection test:

```powershell
& "$env:WINDIR\System32\OpenSSH\sshd.exe" -t
if ($LASTEXITCODE -ne 0) { throw 'Invalid sshd configuration; do not restart' }
Restart-Service sshd
```

From the checkout, generate and review the firewall setup file:

```powershell
py cmake/local-ci/windows_worker.py --print-firewall-setup |
    Set-Content -Encoding UTF8 build-local-ci/windows-firewall-setup.ps1
Get-Content build-local-ci/windows-firewall-setup.ps1
```

Only after the preceding checks, run that reviewed file from an administrator PowerShell on the dedicated host:

```powershell
& .\build-local-ci\windows-firewall-setup.ps1
```

From the actual non-elevated SSH account, verify access to the effective firewall rule and all its filters, as required
by `checkNetwork`. Keep the same account's graphical desktop logged on and unlocked. Run from the orchestrator:

```powershell
py cmake/local-ci/run run --commit HEAD --machines CurmudgeonWindows `
    --windows-kit build-local-ci/windows-1151dc4.tgz `
    --windows-kit-sha256 520f1ec91ec25da36fc8efb1c8ddc25aeab8d90a82f5a0d38bb8b84d653e039e `
    --jobs 4 --require-all
```

The kit matches the dependency pins of the examined commit. Rebuild it if those pins change. A Windows-only run still
reports the eleven-lane matrix as incomplete. Cold/warm runs over real SSH, task registration under the standard account,
disconnect/recovery, public egress checks, multicast/broadcast effects and comparison with GitHub timings remain required.
