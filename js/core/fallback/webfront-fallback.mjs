/// @brief Native module for WebFront's embedded fallback page. Proves the browser-to-C++ bridge
/// connected by awaiting webFront.ready (defined by the classic script WebFront.js, loaded before
/// this module) and reflecting the outcome in the page's status element.

const status = document.getElementById("status");

let bridgeConnected = false;
try {
    await webFront.ready;
    bridgeConnected = true;
    status.textContent = "Connected to WebFront";
    status.dataset.state = "connected";
    const { installDomInterpreter } = await import('./webfront-dom.mjs');
    const dom = installDomInterpreter(webFront);
    dom.island('root', document.getElementById('webfront-root'));
    await dom.connect();
} catch (error) {
    status.textContent = bridgeConnected
        ? `Connected to WebFront; DOM unavailable: ${error.message}`
        : `Connection failed: ${error.message}`;
    status.dataset.state = bridgeConnected ? "dom-error" : "error";
}
