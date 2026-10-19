export function sendToCpp(message) {
    const payload = message || 'Hello from ES modules';
    if (window.webFront && typeof window.webFront.cppFunction === 'function') {
        window.webFront.cppFunction('print')(payload);
        return `Sent to C++: ${payload}`;
    }

    console.log(payload);
    return `Console fallback: ${payload}`;
}

export function installCppBridge(onMessage) {
    globalThis.webfrontModule = {
        receiveFromCpp(message) {
            onMessage(message);
        }
    };

    if (globalThis.webFront && typeof globalThis.webFront.cppFunction === 'function') {
        globalThis.webFront.cppFunction('moduleReady')();
    }
}
