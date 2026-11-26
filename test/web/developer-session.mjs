// A second session made by the proven top-level test view, with its own connection and its own copy of the runtime.
class SessionBridge extends parent.WebFrontBridge {
    cppFunction(name) {
        const call = super.cppFunction(name);
        return (...args) => call(...args.map(value => value instanceof Uint8Array ? new parent.Uint8Array(value) : value));
    }

    executeFunction(name, args) {
        if (name === 'webfront.commands.output') return globalThis.webfront.commands.output(...args);
        if (name === 'webfront.commands.exit') return globalThis.webfront.commands.exit(...args);
        return super.executeFunction(name, args);
    }
}

globalThis.session = (async () => {
    const bridge = new SessionBridge();
    globalThis.addEventListener('pagehide', () => bridge.socket.close(), {once: true});
    await bridge.ready;
    const runtime = await import('/webfront/developer/environment/runtime.mjs');
    return {runtime, bridge, cppCommands: await runtime.discoverCommands(bridge)};
})();
