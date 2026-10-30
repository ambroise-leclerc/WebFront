# Developer distribution 0.6.3 — release preparation

Prepared on 2026-10-29 from the merged automation implementation, `22f6795` (PR #407).
This release branch updates the generated pin and its input checksum. It **does not itself publish
an archive**. Keep the PR as a draft until deployment and the live smoke check pass. The acceptance
report and architecture decision are in PR #411.

## Prepared and verified

- Environment includes automation API v1, shared operation ownership and the final entry-label/dev fixes.
- Four builds (two acceptance outputs, two release outputs including --refresh-bootstrap) produce
  identical environment, Life starter, provision and generated header bytes.
- The generated pin matches the built header. Its URL is
  https://ambroise-leclerc.github.io/WebFront/developer/0.6.3/environment.tgz.
- 121 developer Jasmine specs, real MCP shared-window and official Python SDK 2.3.0 regressions
  pass against these exact candidate bytes. The source baseline passes 357 CEF-off CTest.
- Public 0.6.3 provision was HTTP 404 during preparation. Existing versions are not overwritten.
  Pages is configured for gh-pages, root. No publication or tag has been made by this preparation.
- The test bootstrap was still the separately verified published 0.6.2 when this release was prepared; a later pull request (#403) advances it to the published 0.6.3.

| Archive | Bytes | SHA-256 |
|---|---:|---|
| environment.tgz | 4095568 | fbf4c4036bd084cf4057d4dbc163cabef6444e002e1684896c4be579ad01337a |
| life-starter.tgz | 25497 | 1a79a9d6660a724ed981c17acba5091eda290301279cdf2916414db391ec9e92 |

Environment SRI:

```text
sha512-3AVF2xKsUfHys5VREA6DmkjiEDKjx9VQtPTrsjdiazzfpNQvbKEjW2ox5Kd2hPeOP5MfcMvbDHQ4SZoO97P5Xg==
```

## Publication gate

Follow [PUBLISHING](../../js/developer/tools/PUBLISHING.md), in this order:

1. Review the release commit and its green Developer packages workflow. Download the
   `developer-distribution` artifact and compare the bytes/digests above. The workflow rebuilds
   twice, compares the committed pin and runs the developer specs without Node/npm on the host.
2. Publish that reviewed commit with the dedicated `developer-v0.6.3` tag, or the explicit
   DeveloperDistribution.yml dispatch with publish=true. Tags also archive a dedicated prerelease.
   Do not tag the ADR-only acceptance branch, whose pin deliberately remains on 0.6.2.
3. Verify deployed environment and starter SHA-512 against provision.json and this generated pin,
   and check cross-origin response headers. Do not replace a published version if bytes differ.
4. Run the real-engine public mode of developer_integration as documented in PUBLISHING.
   Record the tag, publish run, URLs, digests and live smoke result in this PR.
5. Only then make the PR ready and merge the pin. No application should default to an unpublished URL.

The candidate artifacts and green packaging checks establish readiness to publish; they are not
a substitute for checking the actual deployed bytes and browser CORS behavior.
