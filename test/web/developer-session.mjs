// A second browser session for developer-specs.mjs: its own WebFront connection, the same environment runtime.
globalThis.session = (async () => {
    await webFront.ready;
    const runtime = await import('/webfront/developer/environment/runtime.mjs');
    return {runtime, bridge: webFront, cppCommands: await runtime.discoverCommands(webFront)};
})();
