# WebFront architecture decision records

This directory describes WebFront’s architectural choices and the constraints to preserve
when making changes or generating code. It contains **15 ADRs**, with unique numbers (12 accepted decisions and 3 proposals).
The accepted baseline corresponds to commit `b031ab5` dated **2026-10-19**;
the browser-powered development proposals are dated **2026-10-20**. Each ADR is versioned
according to the decision’s creation and its significant changes in the Git history.
The ADRs themselves are written in French.

## Reading guide and scope

The format draws on [MduX’s ADR register](https://github.com/ambroise-leclerc/MduX/tree/develop/docs/adr):
context, decision, alternatives, positive and negative consequences, implementation rules
and references. Its structure was consulted in the neighbouring checkout at `MduX/docs/adr`.
MduX’s medical requirements and technology choices do not carry over to WebFront.

The version tables track architectural decisions: **version 1.0** is their initial creation,
dated to the first corresponding commit; subsequent versions record significant changes,
with their Git date, status, author and a link to the commit. Each ADR’s body describes its
latest version. A compatible addition or clarification increments the minor version; replacing
a backend or contract may introduce a major version. The status “Acceptée” (Accepted) means
that the choice recorded in that version has been integrated into the project.

The author of every version is **Ambroise Leclerc**. Dates of accepted historical versions come from the commits
(`git show --format=%as`), searched across all retained branches. The founding intent around
2020 — targeting C++23, anticipating its networking facilities through standalone ASIO and
decoupling web resources from storage — remains described in the relevant ADRs’ context.
Versions use specific Git milestones: they do not invent a day for an intention whose year
alone is known.

Commit `bbe4913`, dated 2025-08-16, brings together earlier work; such an integration does
not itself create a new decision version. The tables record architectural changes;
Git retains the detailed history of fixes and integrations.

WebFront remains experimental. These records do not establish production readiness,
isolation of third-party code or regulatory compliance. Detailed limitations and platform
verification evidence remain in [PROJECT_STATUS](../PROJECT_STATUS.md).

## Index

| ADR | Decision | Status | Initial creation | Current version |
| --- | --- | --- | --- | --- |
| [ADR-001](ADR-001-coeur-cpp23-header-only.md) | A header-only C++23 core embedded in the application | Accepted | 2020-04-19 | 1.5 — 2026-10-13 |
| [ADR-002](ADR-002-systemes-de-fichiers-composables.md) | An abstract, composable filesystem as the foundation for portability | Accepted | 2023-03-16 | 2.4 — 2026-10-14 |
| [ADR-003](ADR-003-protocole-binaire-et-bornes.md) | A shared, typed and bounded binary protocol | Accepted | 2022-02-06 | 1.9 — 2026-10-20 |
| [ADR-004](ADR-004-appels-asynchrones-et-erreurs.md) | Asynchronous calls with deliberately asymmetric contracts | Accepted | 2022-02-06 | 2.4 — 2026-10-18 |
| [ADR-005](ADR-005-evolution-reseau-asio-stdnet.md) | From standalone ASIO to stdnet: anticipating standard networking without blocking progress | Accepted | 2022-01-21 | 3.1 — 2026-10-16 |
| [ADR-006](ADR-006-taches-structurees-et-arret.md) | Owning, cancelling and joining every task | Accepted | 2026-10-13 | 2.0 — 2026-10-18 |
| [ADR-007](ADR-007-frontends-explicites.md) | Explicit frontend selection and a shared lifecycle contract | Accepted | 2026-05-31 | 1.10 — 2026-10-20 |
| [ADR-008](ADR-008-contenu-de-confiance-et-origines.md) | Restricting bridge origins without promising a sandbox | Accepted | 2026-10-18 | 1.0 — 2026-10-18 |
| [ADR-009](ADR-009-modules-et-assets-reproductibles.md) | Serving modules locally and regenerating assets reproducibly | Accepted | 2023-03-16 | 2.0 — 2026-10-19 |
| [ADR-010](ADR-010-composants-et-outils-optionnels.md) | Extending WebFront through optional components and development tools | Accepted | 2026-10-14 | 1.5 — 2026-10-16 |
| [ADR-011](ADR-011-diagnostics-optionnels.md) | Confining mddlog behind an optional diagnostics façade | Accepted | 2026-10-01 | 1.2 — 2026-10-02 |
| [ADR-012](ADR-012-preuves-et-tests.md) | Demonstrating contracts through deterministic tests and browser integration | Accepted | 2020-04-19 | 1.6 — 2026-10-19 |
| [ADR-013](ADR-013-bootstrap-developpeur-a-la-demande.md) | Bootstrapping a downloaded developer environment | Proposée | 2026-10-20 | 0.1 — 2026-10-20 |
| [ADR-014](ADR-014-construction-javascript-dans-le-browser.md) | Resolving npm dependencies and building inside the browser | Proposée | 2026-10-20 | 0.1 — 2026-10-20 |
| [ADR-015](ADR-015-ressources-generees-par-application.md) | Generating application-owned embedded resources | Proposée | 2026-10-20 | 0.1 — 2026-10-20 |

Numbering follows the reading topics; milestones and individual file histories provide
the chronology, which does not follow the numerical order.

## Using the ADRs when making changes or generating code with an LLM

1. Read [AGENTS.md](../../AGENTS.md), ADR-001, ADR-002 and ADR-012, then the ADRs covering the change.
2. Identify object ownership, execution threads, transferable types and failure paths before
   proposing code. Follow each ADR’s implementation rules.
3. Check the referenced API and tests: a name mentioned here does not justify inventing an
   overload, a remote cancellation guarantee or an unsupported result type.
4. Add appropriate verification and keep sources, embedded assets and documentation in sync.
5. If a change contradicts a decision, make that explicit in a new ADR; do not silently
   reintroduce a rejected alternative.

| Planned work | ADRs to read in addition to the 001/002/012 baseline |
| --- | --- |
| Bridge, codecs or new data types | 003, 004, 006, 009 |
| Networking, concurrency, lifetimes or shutdown | 005, 006 |
| Files, resources and modules | 009, 010 |
| A new engine or changes to window lifecycle | 006, 007, 008 |
| Components, the terminal or installed tools | 004, 006, 008, 010 |
| Diagnostics and log transport | 006, 011 |
| Downloaded development environment, npm libraries and generated application assets | 008, 009, 010, 013, 014, 015 |

ADRs explain decisions; [AGENTS.md](../../AGENTS.md) sets out working instructions,
the [README](../../README.md) provides the user journey, and specialist documents define
the detailed contracts. If text and code disagree, report the discrepancy and resolve it
with tests; do not implicitly change the contract to make the contradiction disappear.

## Maintaining the register

Proposals use their actual proposal date and remain proposed until integration/validation.
Each ADR starts with a **Versions** table listing version, date, status, author, change and
commit. Version **1.0** corresponds to the decision’s initial creation, dated to the historical
commit that introduces it. Each significant architectural change adds a row dated to its
corresponding commit, with **Ambroise Leclerc** as the author.
Do not replace historical dates with the date of an editorial change to the document.
Keep previous rows and update the current version in the index.

- Use one `ADR-NNN-description.md` file per decision and one row in this index. The next number
  is **ADR-016**. Retain historical numbers and files.
- For a new decision that has not been integrated, use **Proposée** (Proposed), its actual
  proposal date and validation criteria; do not assign an artificial historical commit date.
- After integration, record the date and supporting commits, the accepted negative
  consequences and the required tests.
- If a decision is replaced, mark it **Remplacée par ADR-NNN** (Superseded by ADR-NNN), link
  in both directions and identify exactly which part changes. A compatible clarification
  may extend an existing ADR’s history without erasing its previous state.
- A future possibility mentioned here is not an accepted decision: general DOM abstractions,
  tool isolation, response deadlines and a decoding cache require their own decisions.

## Chemins depuis la version 0.4

Les exemples publics résident dans `examples/` et les ressources web lisibles dans `assets/`.
Les mentions de `src/` dans les décisions précédentes décrivent l’ancienne organisation ; elles ne
changent pas les contrats. Les démonstrations supprimées restent consultables dans l’historique Git.
