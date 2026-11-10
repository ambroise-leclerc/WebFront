# Developer distribution 0.6.6 — publication

Published on 2026-11-09 from the sources of the automation of a consumer application (PR #502, issue #501), under the tag
`developer-v0.6.6` (workflow run 42604658211), and pinned by `DeveloperReleaseData.hpp`.

## What the environment adds

- The operation `call`, which invokes an application callback that the native owner allow-listed by exact name.
  The native owner refuses the grant on an environment older than 0.6.6.
- The terminal prints the grants and the attach path once automation is connected; `wf help` ends with an
  `Automation:` line while it lasts; the errors asking for typescript or jasmine-core name the `install` operation.
- `wf create` writes an `AGENTS.md` beside the starter's sources.
- `wf check` without a `tsconfig.json` roots the project's `.d.ts`, `.d.mts` and `.d.cts` files.

## Verified

- The environment built on Windows (WebView2 driver) and by the workflow on Linux (WebKitGTK) has the same SRI; the workflow
  built it twice and compared the header with the checked-in one.
- The deployed archive (`https://dist.webfront.fr/developer/0.6.6/environment.tgz`) has the pinned integrity and is
  served with `Access-Control-Allow-Origin: *`; `wf run pin` confirms it.
- The public smoke test (`developer_integration --public`) passed against the deployed files: WASM, npm packages, TSX,
  the Life preview and deterministic generated headers.
- `bundles.json` records the checksum of the pinned header among its inputs.
- The test bootstrap (`cmake/WebFrontBootstrap.cmake`) is not advanced here.

| Archive | SHA-256 |
|---|---|
| environment.tgz | 9f1dccce9d15fd645238bcc8adcbf824c1385d7c451eaed41027b20877c49047 |
| life-starter.tgz | 37e040f95639a15fe21527b180d09873002304a8871263e4fea51f4c3f40d2bd |

Environment SRI:

```text
sha512-sRRxjGZWO4vGNZn0lUw/MPWGx7lV/5WMyITacXpDH3cGRvhCEZJC7p8lLYfjmRn5kBuepfaU40txLJwA5KDbZQ==
```
