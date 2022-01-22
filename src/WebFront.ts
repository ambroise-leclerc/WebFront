
let wfSocket = new WebSocket("ws://localhost", "WebFront_0.1");
wfSocket.onopen = function (event) {
    console.log("wfSocket opened");
    let text = "Voici un texte que le serveur attend de recevoir";
    wfSocket.send(text)
}

wfSocket.onmessage = function (event) {
    console.log("onmessage : " + event.data);
}

wfSocket.onclose = function (event) {
    if (event.wasClean) console.log(`[close] Connection closed cleanly, code=${event.code} reason=${event.reason}`);
    else console.log('[close] Connection died');
    
}

wfSocket.onerror = function (error) {
    console.log(`[error] ${error}`);
}
