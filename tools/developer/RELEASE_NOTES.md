Browser-powered WebFront developer environment with Game of Life and PI starters, versioned separately from the C++ library.

Version 0.2.0 adds a C++23 bitset Game of Life demo with an isometric React/canvas interface, translucent panels, editable patterns, playback and single stepping. Each generation is computed in C++ through the asynchronous bridge. The PI starter and version 0.1.0 remain available.

Download through the version-pinned GitHub Pages URLs in the bootstrap configuration. SHA-512 is verified before executing the environment or creating a starter. React, React DOM and scheduler remain on npm; application developers do not install Node.js or npm. Both starters can generate their own compressed header-only resources for an offline native executable.

The environment includes the terminal, npm resolver, esbuild/WASM compiler, embedding tools and dependency notices. These packages are experimental; browser packages only, without installation scripts, native modules or Node API shims.
