# Publishing the developer packages

The version and canonical HTTPS base URL are in `distribution.json`. The environment and the Life starter
are published under `developer/<version>/`. React, React DOM and scheduler stay on npm; this site
hosts only WebFront's development environment, its pinned Life starter and provisioning metadata.
The package names in their manifests are identifiers, not a requirement to own or publish npm scopes.

The environment builds itself: the pinned bootstrap environment (`cmake/WebFrontBootstrap.cmake`) runs the scripts of
`js/developer/scripts/` on the sources of this repository, which is a `wf` project (`js/developer/webfront.project.json` and its
lock). Building, reviewing and publishing need a C++23 compiler, CMake, the engine of the driver's frontend and `gh`
and `jq` for the host operations.

## Releasing with the workflow

The **release-developer** workflow (`.github/workflows/ReleaseDeveloper.yml`, ADR-021) is the way to publish a version. Dispatch
it on `develop` with the version (greater than the one of `distribution.json`):

```sh
gh workflow run release-developer.yml --ref develop -f version=0.6.7 -f publish=false   # rehearsal
gh workflow run release-developer.yml --ref develop -f version=0.6.7 -f publish=true    # release
```

The jobs run in this order, and each declares only the permissions of the table of ADR-021 (`ReleaseDeveloperWorkflow` checks them):

1. `wf-package` (`contents: read`): sets the version, builds twice with `--refresh-bootstrap`, compares the repeat build and the
   header, renders the embedded headers and the **unreleased** row of the compatibility table, fails if the diff touches a path
   outside `RELEASE_PATHS` (declared once in the workflow; `check-release-paths.sh`), checks the pin (`CheckDeveloperPin.cmake`)
   and runs the specs. With `publish=false` the workflow ends here: nothing is pushed, tagged or published.
2. `prepare`: with the token of the dedicated GitHub application, commits that closed list of paths on
   `release/developer-<version>` (never `develop` or `master`); an existing branch stops the run.
3. `tag`: sets `developer-v<version>` on that commit, in the same run.
4. `publish`: stages the site on `gh-pages` (immutable versions, no force-push), requests the Pages build and archives the
   prerelease.
5. `verify`: `wf run pin` (bytes, pinned URL without redirection, CORS; retried while Pages builds) and the public smoke test.
6. `pull-request`: with the application's token, opens the pull request of the release branch into `develop`. It is not merged by
   the workflow: the review and the required checks of `develop` apply, and they run because the pull request is not created
   with the `GITHUB_TOKEN`.

**Resuming.** A failure of `wf-package` or `prepare` leaves nothing pushed or deployed (delete the release branch if `prepare`
pushed it) and the same version can be dispatched again. Once `tag` has run, the tag rule `developer-tags` forbids moving or
deleting `developer-v<version>`: a failure of `tag` or `publish` after it is resumed by re-running the failed jobs of the same
run, which publishes the same bytes, and a defect found at that point requires the next version. A failure of `verify` after `publish` is a transient fault if
`wf run pin` succeeds on a rerun (re-run the failed jobs: the bytes are the same) and a defect otherwise, which requires a new
version. A failed `verify` opens no pull request and changes neither `develop` nor `master`.

`pin.mjs` (`wf run pin`) fails when the pinned URL is not on the origin of `baseUrl`, redirects, or lacks CORS headers. The only
exception is a migration, declared by `previousBase` in `distribution.json` while the header still pins a version published
under that address; it prints a warning, and is refused as soon as the header pins a version published under the new address.

**Repository settings the workflow needs** (made by a maintainer, not by the workflow):

- a GitHub application with `pull-requests: write` and `contents: write` only, installed on this repository; its identifier is
  the variable `RELEASE_APP_ID` and its private key the secret `RELEASE_APP_PRIVATE_KEY` of the protected environment
  `developer-release-app`;
- a tag rule on `developer-v*` forbidding updates and deletions;
- the branch protection of `develop` and `master` must not cover `release/*`, and the application has no bypass;
- the Pages source and the environment `developer-pages-publication` unchanged ("One-time Pages configuration" below).

The library's release preparation freezes the compatibility table with `cmake -DROOT=. -DMODE=freeze -DLIBRARY_VERSION=<x.y.z> -P
cmake/DeveloperCompatibility.cmake`; `CheckReleaseVersion.cmake` fails a `v*` tag without its row.

The sections below describe the building blocks and the manual procedure, which remains the fallback when the workflow cannot run.

## Build and review

The driver `webfront-wf` runs one `wf` command without interaction on a project (here, the repository). Any frontend
works; WebKitGTK on Linux is the one CI uses.

```sh
cmake -S . -B build-driver -DCMAKE_BUILD_TYPE=Release -DWEBFRONT_ENABLE_TESTING=ON \
  -DWEBFRONT_EMBED_WEBKITGTK=ON -DWEBFRONT_WEBTEST_FRONTEND=WebKitGTK
cmake --build build-driver --target webfront-wf
wf() { xvfb-run -a build-driver/test/web/webfront-wf --project "$PWD" --manifest-dir js/developer -- "$@"; }

wf restore                                    # the locked build dependencies, verified against their SHA-512
wf run build build-distribution               # environment, starters, provision.json, pinned header
wf run build build-distribution-repeat        # must be the same bytes
cmp build-distribution/environment.tgz build-distribution-repeat/environment.tgz
```

`wf run build <directory> --refresh-bootstrap` also rebuilds the checked-in bootstrap bundles
(`js/core/bootstrap/bootstrap.bundle.mjs`, `js/developer/environment/packages.bundle.mjs`), their checksums
(`js/developer/environment/bundles.json`, over the inputs of `js/developer/tools/bundle-inputs.json`) and the pinned
header. The core headers that embed the bootstrap bundle (`DeveloperIndexHtmlData.hpp`, `DeveloperBootstrapData.hpp`) are
rendered from their sources by CMake, like the others: `cmake --build <dir> --target webfront-embedded-assets`. A script may only write where `permissions.publish`
of `js/developer/webfront.project.json` allows: the `build-*` directories and these checked-in outputs.

The output of a script reaches the terminal of an environment 0.6.1 or later; with 0.6.0 the command prints
only its summary. The integrity of an archive is `openssl dgst -sha512 -binary build-distribution/environment.tgz | base64 -w0`,
prefixed by `sha512-`.

Review and commit the updated checked-in bootstrap bundles and checksums. Regenerating
`include/webfront/system/DeveloperReleaseData.hpp` is part of the release procedure ("Pinning a new version"): the header
is committed on a release branch before the tag, because the publication workflow requires it to match the build, and it
reaches the default branch only after the deployed files are verified, so that the default bootstrap never names an
archive that is unavailable. The header pins the exact environment archive and URL.
The environment pins the starter archive in turn. `provision.json` is informational; applications
do not discover a replacement trust hash from that remote file or follow `latest` at startup.

The static branch preserves every version. Republishing identical bytes is allowed; replacing a
published version with different bytes fails. Increment `distribution.json` before publishing any
changed environment, starter, public URL or provisioning configuration.

## Pinning a new version (manual fallback)

`distribution.json` names the version under development; `DeveloperReleaseData.hpp` pins the last version that is
published. Pull requests and branch pushes therefore require the header to match a fresh build only when both name
the same version; a publication (a `developer-v*` tag or a dispatch with `publish=true`) always requires it. When the
sources are ahead, `wf run pin` requires the header to be exactly the one generated for its version and the deployed
archive at its URL to have its integrity.

A release preparation branch may carry the new pin before the archive exists (for instance
[0.6.3](../../docs/validation/developer-0.6.3-release.md)). Keep its pull request as a draft and
merge it only after the deployed bytes and headers are verified.

1. Merge the changes with the header still on the previous version.
2. On a release branch, run `wf run build build-distribution --refresh-bootstrap` and commit the new header (and the
   checksums of `bundles.json`, which cover it).
3. Tag `developer-v<version>` on that commit; the workflow publishes the site.
4. Verify the deployed files (below), then merge the release branch so the default bootstrap points to the new version.

Once the new version is published, `cmake/WebFrontBootstrap.cmake` may pin it as the bootstrap environment of the
tests, in a pull request of its own.

## Domain

The distribution is served under `https://dist.webfront.fr/` (ADR-021): `baseUrl` of `distribution.json`, hence the URL in
every pinned header. The zone of `webfront.fr` holds a `CNAME` from `dist` to `ambroise-leclerc.github.io.`, the domain
`webfront.fr` is verified in the account's Pages settings, and HTTPS is enforced. `wf run site` writes the `CNAME` file of
the Pages branch from `baseUrl`; the workflow removes it when `baseUrl` returns to a github.io address. A custom domain must
serve the root of the site. Changing the base URL is a public URL change: increment the version, and expect every
earlier pin on the former address to stop working in a browser, since Pages redirects without CORS headers.

## One-time Pages configuration

The repository's Pages source must be the **gh-pages branch, root directory**, with branch-based
builds (`build_type: legacy`). The branch contains only generated distribution files and `.nojekyll`.
An existing unrelated Pages site must not be overwritten.

For initial setup, a repository maintainer first commits/pushes the generated site to a new
`gh-pages` branch, then enables Settings → Pages → Deploy from a branch → gh-pages → `/`.
The equivalent API call, after that branch exists, is:

```sh
gh api --method POST repos/ambroise-leclerc/WebFront/pages \
  -f build_type=legacy -f 'source[branch]=gh-pages' -f 'source[path]=/'
```

The publication workflow uses repository tokens with `contents: write` and `pages: write` to update
that branch without force-pushing. It explicitly requests a Pages build, because token-driven
branch pushes do not trigger one automatically. It refuses to change an existing Pages source.
Initial Pages enablement is deliberately a maintainer operation, not a side effect of every build.

## Automated publication

The **Developer packages** workflow builds packages on relevant pushes/PRs and retains them as an
artifact. Its `wf-package` job removes every JavaScript runtime from the runner, builds the driver, restores the locked
dependencies, builds the distribution twice and compares the repeat build and the checked-in bootstrap header before
permitting publication; it also runs the specs of the environment with `wf test`.
It publishes only on a `developer-v<version>` tag or an explicit workflow dispatch with `publish=true`. The `publish` job
runs the same way: `wf run site` stages the new version into a checkout of `gh-pages` and refuses to
change a published one, `pages.sh` checks the configured source and requests the build, and `archive-release.sh`
archives the files in a prerelease.
Use the dedicated tag `developer-v<version>` matching `distribution.json`; the C++ library's `v*` releases remain separate.

```sh
version=$(jq -r .version js/developer/tools/distribution.json)
git tag "developer-v$version"
git push origin "developer-v$version"
```

Alternatively, once the workflow exists on the default branch, run it manually for a reviewed ref.
Tagged publication also archives the same files in a dedicated GitHub prerelease. Existing release
assets are verified on retry; they are never replaced. Downloads used by the bootstrap come from
Pages, whose cross-origin behavior is checked independently of Release asset redirects.

## Validate the deployed files

Check the canonical Pages URL, its `Access-Control-Allow-Origin` response and the archive checksum.
Then run the optional live smoke test (the ordinary CTest suite remains entirely offline). Its fixtures (the local
registry and the test pages) are the ones that the CTest case `DeveloperFixtures` assembles in the build directory:

```sh
ctest --test-dir build-native -R '^DeveloperFixtures$' --output-on-failure
./build-native/test/web/developer_integration \
  "$PWD/build-native/test/web/developer-fixtures" "$PWD/build-native/test/web/public-generated" --public
```

This mode downloads the pinned environment and the starter from Pages, fetches exact React/React DOM
versions and scheduler from npm, executes esbuild/WASM, creates the in-window Life preview and generates
the application provider. A mismatch, download failure, browser error or failed assertion returns
nonzero. Keep the issue open until this real-engine check and the normal offline regression suite pass.
