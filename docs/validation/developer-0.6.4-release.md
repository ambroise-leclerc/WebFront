# Developer distribution 0.6.4 — release preparation

Prepared on 2026-11-02 from the merged setup operations of the automation runner (PR #472, #466).
This release branch updates the generated pin and its input checksum. It **does not itself publish
an archive**. Keep the PR (#476) as a draft until deployment and the live smoke check pass.

## Prepared and verified

- Environment adds the project setup operations (`init`, `create`, `install`, `restore`, `embed`) to the automation
  runner; the native owner refuses a setup grant on an environment older than 0.6.4.
- Two local builds produce identical environment, starter, provision and generated header bytes.
- The generated pin matches the built header; `bundles.json` records its input checksum
  (`aec3b6ce948861fce49a481784cf61ef1bc7d30e3fbb23f2d41d3a561cb3a241`). Its URL is
  https://ambroise-leclerc.github.io/WebFront/developer/0.6.4/environment.tgz.
- The local WebKitGTK run passed 122 developer specs.
- Existing versions are not overwritten. No publication or tag had been made at preparation time.
- The test bootstrap (`cmake/WebFrontBootstrap.cmake`) stays on the published 0.6.3; a separate pull request advances
  it once 0.6.4 is deployed and verified.

| Archive | SHA-256 |
|---|---|
| environment.tgz | 2a15c8c2c4df700012d561667488ac6f4584d018a4964a6cd85ec164949144c4 |
| life-starter.tgz | 8b33ddeb2a50501f6c52090ed7a5fd6f48ece9dce81207bc640099daf76fb2e9 |

Environment SRI:

```text
sha512-FJe6RqVjsLEKgfnSONDCbfk2wsGfkGzzgV0q33NIR+DYZpVvxakL/cxsVwi67eQwRuHI6ivlnV9D/oV92xTNCg==
```

## Publication gate

Follow [PUBLISHING](../../js/developer/tools/PUBLISHING.md), in this order:

1. Review the release commit and its green Developer packages workflow. Download the
   `developer-distribution` artifact and compare the digests above.
2. Publish that reviewed commit with the dedicated `developer-v0.6.4` tag, or the explicit
   DeveloperDistribution.yml dispatch with publish=true.
3. Verify the deployed files (HTTP 200, CORS) and the public browser smoke test.
4. Only then mark the PR ready and merge; the test bootstrap bump follows in its own pull request.

## Publication verified

- Tag [`developer-v0.6.4`](https://github.com/ambroise-leclerc/WebFront/releases/tag/developer-v0.6.4)
  points to the candidate commit `48da31c`. The [publication workflow](https://github.com/ambroise-leclerc/WebFront/actions/runs/41403355294)
  rebuilt the packages, passed the developer specs and published the Pages files and release assets.
- The environment, starter and provision files returned HTTP 200 with `Access-Control-Allow-Origin: *` and matched
  the locally built files byte for byte. Their SHA-256 values match the table above.
- The public WebKitGTK smoke test passed using the 0.6.4 pin: it downloaded the published environment and starter,
  installed the locked React dependencies, ran the Life preview and bridge, and generated the embedded headers.
