# @webfront/react

Optional React adapter for WebFront. It is installed with `wf`, uses React's
`useSyncExternalStore`, and renders C++ DOM islands without a custom reconciler.
Neither React nor this package is added to the core's embedded resources.

The package sources, declared files and license are self-contained in `js/react`.
The packaged license is checked against the repository's root `LICENSE`.
The peer ranges accept React `^19.3.1` and optional `@types/react` `^19.3.0`;
CI currently tests 19.3.1 and 19.3.0 respectively. From the repository's wf project, run
`wf run react-package` after `wf install` to generate
`build-react-package/webfront-react-0.1.0.tgz` and its SHA-512.
This prepares an npm archive; it does **not** publish it. Install from a registry
carrying that archive with `wf install @webfront/react@0.1.0 react@19.3.1 react-dom@19.3.1`.
The repository's test registry packages these same sources and proves installation
without public network access. See [React integration](https://github.com/ambroise-leclerc/WebFront/blob/develop/docs/REACT.md) for
the C++ API, lifecycle contract, and Game of Life variant.
