# WebFront website

Static French and British English website, including an interactive Game of Life and a short practical book (eight chapters and three optional concept detours). Generated HTML works without JavaScript; the simulation, quizzes, copy buttons, search and local reading progress are progressive enhancements.

## Work locally

From `docs/website`:

```sh
node build.mjs
node check.mjs
node serve.mjs
```

Open `http://127.0.0.1:4173/fr/` or `/en/`. Node is only a local authoring tool, not a hosting requirement. No packages are installed. The site uses system fonts and makes no third-party asset requests. All public files are in `dist/`. Content and code examples live in `content.mjs`; HTML structure lives in `build.mjs`. CSS, browser JavaScript and demonstration logic are authored directly under `dist/assets/`. Rebuilding preserves these assets. The two real-application images come from `docs/images/`; refresh their copies when the source walkthrough changes. The tricolour swallow and its compact icon are maintained in `docs/images/webfront-swallow.svg` and `docs/images/webfront-icon.svg`; the build copies them to the site assets. The compact icon retains the forest silhouette, lime throat and explicit white neck separator on a pale tile for light and dark browser chrome.

`node check.mjs` checks local page and asset targets, language tags, page count and Conway behaviour (oscillator, toroidal boundary and glider translation). Browser QA should also cover play/pause, pattern changes, reset, view changes, correct/incorrect quiz answers, reading progress across reloads, search, language switching and narrow-screen layout.

## Deploy to OVH

For the saved Cyberduck connection and repeat-update procedure, see [DEPLOYMENT.fr.md](DEPLOYMENT.fr.md#reconnexion-et-mises-à-jour).

See [HOSTING.fr.md](HOSTING.fr.md) for the hosting assessment and deployment procedure. Transfer the **contents** of `dist/` to the document root configured for `webfront.fr`, preserving directories. The site expects to be at the domain root. No server-side build, CMS, database or catch-all rewrite is needed: every chapter has its own `index.html`.

For future releases, preserve any existing developer-package directories and back up the current files before uploading. Do not remove or replace the existing `/developer/` distribution. `js/developer/scripts/site.mjs` stages the developer distribution separately and writes a package index: never run it against the new website document root without isolating its output first, because it overwrites `index.html`.

Before the public cutover, confirm HTTPS on both apex and `www`, choose the canonical hostname, test direct chapter URLs, replace the current GitHub redirect, and check the real 404 response. Absolute canonical/Open Graph URLs, language alternatives, robots.txt and a sitemap target https://webfront.fr. See DEPLOYMENT.fr.md for the actual OVH transfer, DNS changes and certificate status.

The `.openai/hosting.json` identity is for the separate private review deployment. It is not required on OVH. Keep source publishing checkouts outside this repository so that they do not introduce nested Git repositories.

## 0.6.0 and 0.7.0 content update

The book includes C++ DOM/forms/React patterns, local MCP and CLI harness integration, and a complete consumer Jasmine journey (quotation page, native app, specs and CMake). The generated `dist/examples/quote/` sources match the displayed snippets. The native example targets macOS WKWebView; other platforms must select and configure their own embedded engine. Public reference links are pinned to v0.7.0, and developer environment 0.6.3 is versioned separately.

The main reading path now has eight chapters; progress derives its count from navigation and preserves previously read chapter IDs.

## Editorial conventions

See [STYLE.fr.md](STYLE.fr.md). Inline programme names use backticks; first definitions can use single-asterisk italics. Blank lines separate paragraphs; groups of `- ` lines form lists. `prose.mjs` renders this restricted notation and escapes HTML. Metadata uses plain text. The book uses a serif reading face and distinct monospace inline code; the site uses the same notation for technical references.
