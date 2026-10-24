# Publishing the developer packages

The version and canonical HTTPS base URL are in `distribution.json`. The environment and the Life starter
are published under `developer/<version>/`. React, React DOM and scheduler stay on npm; this site
hosts only WebFront's development environment, its pinned Life starter and provisioning metadata.
The package names in their manifests are identifiers, not a requirement to own or publish npm scopes.

The environment builds itself: the pinned bootstrap environment (`cmake/WebFrontBootstrap.cmake`) runs the scripts of
`js/developer/scripts/` on the sources of this repository, which is a `wf` project (`js/developer/webfront.project.json` and its
lock). Building, reviewing and publishing need a C++23 compiler, CMake, the engine of the driver's frontend and `gh`
and `jq` for the host operations.

## Build and review

The driver `webfront-wf` runs one `wf` command without interaction on a project (here, the repository). Any frontend
works; WebKitGTK on Linux is the one CI uses.

```sh
cmake -S . -B build-driver -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTING=ON \
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

## Pinning a new version

`distribution.json` names the version under development; `DeveloperReleaseData.hpp` pins the last version that is
published. Pull requests and branch pushes therefore require the header to match a fresh build only when both name
the same version; a publication (a `developer-v*` tag or a dispatch with `publish=true`) always requires it. When the
sources are ahead, `wf run pin` requires the header to be exactly the one generated for its version and the deployed
archive at its URL to have its integrity.

1. Merge the changes with the header still on the previous version.
2. On a release branch, run `wf run build build-distribution --refresh-bootstrap` and commit the new header (and the
   checksums of `bundles.json`, which cover it).
3. Tag `developer-v<version>` on that commit; the workflow publishes the site.
4. Verify the deployed files (below), then merge the release branch so the default bootstrap points to the new version.

Once the new version is published, `cmake/WebFrontBootstrap.cmake` may pin it as the bootstrap environment of the
tests, in a pull request of its own.

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
