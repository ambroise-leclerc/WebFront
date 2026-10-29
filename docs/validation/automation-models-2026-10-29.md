# Local-model automation qualification — 2026-10-29

This measurement exercises the real developer window and native MCP controller described in
[Automation](../AUTOMATION.md). The implementation is PR #407 at `b1fe8ac`, with the bootstrap
exit-status correction included with this report. [Recorded data](automation-models-2026-10-29.json)
contains settings, per-task outcomes, token counters and conversation analysis for all 30 attempts.

## Setup and method

The five models were already installed in LM Studio. Inference used its loopback OpenAI-compatible
API and `llama.cpp-linux-x86_64-amd-rocm-avx2@2.66.0`, on an AMD Radeon AI PRO R9700 with about
32 GiB VRAM. Each model was loaded alone with maximum GPU offload and a 16,384-token context.
All five variants use Q4_K_M quantization. No model or inference runtime was downloaded.

The maintained [benchmark](../../test/tools/AutomationModelBenchmark.py) still speaks Ollama's
chat protocol. This experiment used an external measurement adapter for LM Studio; pointing
`--ollama-url` at an OpenAI-compatible endpoint alone does not reproduce it. The adapter:

- projected the same trusted MCP catalogue into function tools, adding `properties: {}` to empty
  object schemas as required by that endpoint;
- converted argument dictionaries to JSON strings and associated tool replies with their call IDs;
- preserved returned reasoning content and mapped `usage` counters to the benchmark's counters;
- retained the loopback-only HTTP policy, refused proxies/redirects and bounded responses to 1 MiB;
- never interpreted a Markdown/JSON text answer as a tool invocation.

The MCP tasks, prompts and verification logic stayed the same. Each fresh generated project had
either a TypeScript type error (`count: number = '42'`) or a failing spec (`count = 41`, expected 42).
The model could read/write only generated `main.ts` through fixture tools, and use the four MCP
tools with native `check` and `test` grants. It could not edit the spec or manifest through those tools.
The browser was WebKitGTK under Xvfb, running the rebuilt developer environment and local registry.

Each task ran three times per model: six attempts per model, 30 total. Settings were temperature 0,
at most 24 turns and 512 generated tokens per task response. Catalogue measurements used identical
baseline messages with/without the four MCP tools and one generated token. Fixture tools were
excluded from the catalogue delta. Models were unloaded between series.

## Outcomes

| Installed model | Final source passes host check/test | Model observed both check/test succeed | Mean task time (s) | MCP catalogue token delta | Mean prompt tokens across turns | Mean generated tokens |
|---|---:|---:|---:|---:|---:|---:|
| Devstral Small 2 24B | 6/6 | 6/6 | 16.71 | 553 | 11,603 | 355 |
| Qwen 3.8 27B | 6/6 | 6/6 | 32.30 | 729 | 14,556 | 1,239 |
| Muse Glimmer 30B | 6/6 | 6/6 | 64.98 | 837 | 20,914 | 1,210 |
| Flint 40B | 6/6 | 5/6 | 51.16 | 559 | 25,674 | 1,253 |
| Nemotron Cascade 2 30B-A3B | 6/6 | 6/6 | 18.81 | 811 | 20,586 | 1,379 |

The host independently checked the final source and ran tests after the model stopped: **30/30**
passed, with no verification transport errors. The separate conversation analysis counts only
`succeeded` results the model received for both operations in its own conversation: **29/30**.
The benchmark's original success metric measures final source correctness; it does not require the
model to complete its own verification. Both metrics are needed to interpret these results.

Observed recoveries and limits:

- Qwen started check/test together in each attempt. The controller refused one start with `busy`;
  the model recovered and observed both operations succeed in all six attempts.
- Nemotron supplied `entry` to `check` once per attempt. Native validation returned
  `invalid_arguments: Unexpected operation argument: entry`; it corrected the arguments and
  observed both operations succeed in all six attempts.
- Flint encountered `wrong_session` in one attempt. Its source was already correct, but its
  reasoning reached the 512-token response limit before it confirmed both successful operations.
  Host verification passed; that conversation remains incomplete in the stricter metric.

The original MCP catalogue measures 1,872 bytes. Token deltas measure its actual provider-specific
projection, not a universal tokenizer cost. Prompt counters sum full reported prompts across turns,
including repeated context; completion counters include reasoning. Mean task time starts after model
loading, browser bootstrap and catalogue measurement, and includes independent host verification.
It does not measure complete startup or shutdown latency.

These two synthetic tasks and three repetitions per model are limited evidence, not a general
reliability or safety guarantee. The backend and tool templates differ from the earlier Ollama
experiments, so changes in outcomes cannot be attributed solely to model age. This qualification
does not accept ADR-019 as a whole or establish compatibility with every MCP client.

## Bootstrap exit-status regression and native validation

The CLI previously classified the same bootstrap failure as either exit 70 or exit 1. Its transport
could report the closed session first, setting the generic failure status to 1 before the main
thread classified the bootstrap failure. Three identical process launches returned `70, 1, 70`.

`recordBootstrapFailure` now replaces 0 or 1 with 70 using compare-exchange, preserving specific
native deadline, forced-stop and signal statuses. The native regression checks both transport
completion orders and preservation of 71, 72, 130 and 143. With the original classification logic,
the extracted scenario fails one of six assertions; with the correction, all six pass. The real CLI
watchdog regression now requires exactly 70 for bootstrap failure. Repeating the original process
reproduction returns `70, 70, 70`.

Linux validation with the correction: **355/355 CTest without CEF**, **90/90 CEF/Xvfb integration
cases**, and **118 developer Jasmine specs**. The GCC 16.1 CEF-off build needed existing unrelated
warnings made nonfatal: `null-dereference` from stdexec through `DomDocumentTests`, and `nonnull`
only when compiling `ReactStoreTests`. Those local allowances are not project changes; this is not
a successful strict GCC 16.1 baseline build. The CEF launcher targets retained project warning flags.
Historical Windows/macOS and sanitizer results on `b1fe8ac` do not validate the subsequent correction.

## Resource scope

Preparation runs with insufficient free GPU memory were excluded. The retained series began with
about 1.5 GiB GPU occupancy and used maximum GPU offload for every model. No model was left loaded:
the loaded-model list was empty after the series, the temporary LM Studio API was stopped, and no
Ollama or llama-server process remained. Final GPU occupancy was about 1.58 GiB.
