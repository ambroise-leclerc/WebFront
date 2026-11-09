# @webfront/services

Browser side of the optional WebFront system services (see ADR-020). It is installed with
`wf`, bundled into the application by `wf build`, and embedded by `wf embed`; it is never added
to the core's embedded resources. The C++ counterparts will live under `include/webfront/services/`
as the services are delivered.

This release is a skeleton: `services` is empty and `selfTest()` returns the package name, which
proves the delivery chain. From the repository's wf project, run `wf run services-package` after
`wf install` to generate `build-services-package/webfront-services-0.1.0.tgz` and its SHA-512.
This prepares an npm archive; it does **not** publish it. Install it from a registry carrying
that archive with `wf install @webfront/services@0.1.0`. The repository's test registry packages
these same sources and proves installation without public network access.
