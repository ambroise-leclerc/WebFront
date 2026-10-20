# Publishing the developer packages

The version and canonical HTTPS base URL are in `distribution.json`. The environment and Life/PI starters
are published under `developer/<version>/`. React, React DOM and scheduler stay on npm; this site
hosts only WebFront's development environment, its pinned Life/PI starters and provisioning metadata.
The package names in their manifests are identifiers, not a requirement to own or publish npm scopes.

## Build and review

```sh
npm ci --prefix tools/assets --ignore-scripts
node tools/developer/build.mjs build-distribution --refresh-bootstrap
node tools/developer/fixtures.mjs build-distribution
node --test tools/developer/check.mjs tools/developer/distribution-check.mjs
node tools/developer/site.mjs build-distribution build-developer-site
```

Review and commit the updated checked-in bootstrap bundles, checksums and
`include/system/DeveloperReleaseData.hpp`. The header pins the exact environment archive and URL.
The environment pins the starter archive in turn. `provision.json` is informational; applications
do not discover a replacement trust hash from that remote file or follow `latest` at startup.

The static branch preserves every version. Republishing identical bytes is allowed; replacing a
published version with different bytes fails. Increment `distribution.json` before publishing any
changed environment, starter, public URL or provisioning configuration.

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
artifact. It compares a repeat build and the checked-in bootstrap header before permitting publication.
It publishes only on a `developer-v<version>` tag or an explicit workflow dispatch with `publish=true`.
Use a dedicated tag such as `developer-v0.2.0`; the C++ library's `v*` releases remain separate.

```sh
git tag developer-v0.2.0
git push origin developer-v0.2.0
```

Alternatively, once the workflow exists on the default branch, run it manually for a reviewed ref.
Tagged publication also archives the same files in a dedicated GitHub prerelease. Existing release
assets are verified on retry; they are never replaced. Downloads used by the bootstrap come from
Pages, whose cross-origin behavior is checked independently of Release asset redirects.

## Validate the deployed files

Check the canonical Pages URL, its `Access-Control-Allow-Origin` response and the archive checksum.
Then run the optional live smoke test (the ordinary CTest suite remains entirely offline):

```sh
./build-native/webtest/developer_integration \
  "$PWD/build-distribution" "$PWD/build-native/webtest/public-generated" --public
```

This mode downloads the pinned environment and both starters from Pages, fetches exact React/React DOM
versions and scheduler from npm, executes esbuild/WASM, creates in-window PI and Life previews and generates
both application providers. A mismatch, download failure, browser error or failed assertion returns
nonzero. Keep the issue open until this real-engine check and the normal offline regression suite pass.
