/// @brief Native module for WebFront's embedded fallback page. Proves the browser-to-C++ bridge
/// connected by awaiting webFront.ready (defined by the classic script WebFront.js, loaded before
/// this module) and reflecting the outcome in the page's status element.

const status = document.getElementById("status");

try {
    await webFront.ready;
    status.textContent = "Connected to WebFront";
    status.dataset.state = "connected";
} catch (error) {
    status.textContent = `Connection failed: ${error.message}`;
    status.dataset.state = "error";
}
