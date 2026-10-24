Browser-powered WebFront developer environment with Game of Life starter (the PI starter of earlier versions is retired), versioned separately from the C++ library.

Version 0.3.0 migrates the Life starter to packed Uint8Array grids: 128 bytes per generation with direct C++ bitset registration. It requires the bridge support introduced in #289. The interface retains editable patterns, playback, reset and single stepping. Version 0.3.1 makes `wf install` honor the installed lock: versions it records are kept and their archives must still match the recorded integrity. Published versions 0.1.0 to 0.3.0 remain available without modification.

Download through the version-pinned GitHub Pages URLs in the bootstrap configuration. SHA-512 is verified before executing the environment or creating a starter. React, React DOM and scheduler remain on npm; application developers install no JavaScript tooling. The starter can generate their own compressed header-only resources for an offline native executable.

The environment includes the terminal, npm resolver, esbuild/WASM compiler, embedding tools and dependency notices. These packages are experimental; browser packages only, without installation scripts, native modules or server-side runtime shims.

Version 0.4.0 updates the Life starter text for a developer audience. The simulation and bridge contract are unchanged.

Version 0.5.0 introduces the project manifest `webfront.project.json`: `wf init`, manifest-driven `wf install` (with `--update`), `wf preview` and `wf embed`, and a lock that records the digest of the manifest dependencies. `wf install` now requires a manifest. The starters ship their own manifest.

Version 0.6.0 (published as `developer-v0.6.0`) adds `wf test` (the project's Jasmine, obtained by `wf install jasmine-core`, runs the specs of `test.specs` in the browser, with a structured report and a non-interactive entry), `wf build` and incremental `wf dev`, with bounded input polling, diagnostics in the preview and Ctrl+C cancellation. A publication whose commit has completed is displayed before cancellation ends the command. `DeveloperReleaseData.hpp` and the bootstrap environment of the tests pin it. `environment.json` now declares `api` (the `webfront.developer.*` version, 1); an environment that predates the field reads as 1.
