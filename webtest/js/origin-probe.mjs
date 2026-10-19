// A rejected handshake must finish closing before the owner removes its document.
// Successful controls remain open until their owner tears down the test suite.
export function watchOriginProbe(url, report, WebSocketType = WebSocket) {
    const socket = new WebSocketType(url, 'WebFront_0.1');
    let opened = false;
    socket.onopen = () => { opened = true; report('opened'); };
    socket.onclose = () => { if (!opened) report('rejected'); };
    return socket;
}
