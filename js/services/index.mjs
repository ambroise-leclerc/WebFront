// Browser side of the optional WebFront system services (ADR-020). The package is installed with wf and bundled into
// the application; none of it belongs to the core's embedded resources. The services themselves (files, dialogs...)
// are added here as their C++ counterparts are delivered: the skeleton only proves the delivery chain.

/** The services of this package, by name: none yet. */
export const services = Object.freeze({});

/** Proves that the package was installed, resolved and bundled: returns its own name. */
export function selfTest() {
    return '@webfront/services';
}
