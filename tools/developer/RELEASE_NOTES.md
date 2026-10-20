Browser-powered WebFront developer environment with Game of Life and PI starters, versioned separately from the C++ library.

Version 0.3.0 migrates the Life starter to packed Uint8Array grids: 128 bytes per generation with direct C++ bitset registration. It requires the bridge support introduced in #289. The interface retains editable patterns, playback, reset and single stepping. Version 0.3.1 makes `wf install` honor the installed lock: versions it records are kept and their archives must still match the recorded integrity. Published versions 0.1.0 to 0.3.0 remain available without modification.

Download through the version-pinned GitHub Pages URLs in the bootstrap configuration. SHA-512 is verified before executing the environment or creating a starter. React, React DOM and scheduler remain on npm; application developers do not install Node.js or npm. Both starters can generate their own compressed header-only resources for an offline native executable.

The environment includes the terminal, npm resolver, esbuild/WASM compiler, embedding tools and dependency notices. These packages are experimental; browser packages only, without installation scripts, native modules or Node API shims.
