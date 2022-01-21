"use strict";
function greet(person: string, date: Date) {
    console.log('Hello ${person}, today is ${date.toDateString()}!')
}

greet("Brendan", new Date());

let wfSocket = new WebSocket("ws://localhost", "WebFront_0.1");
wfSocket.onopen = function (event) {
    console.log("wfSocket opened");
    wfSocket.send("Voici un texte que le serveur attend de recevoir");
}

wfSocket.onmessage = function (event) {
    console.log("onmessage : ${event.data}")
}

wfSocket.onclose = function (event) {
    if (event.wasClean) console.log(`[close] Connection closed cleanly, code=${event.code} reason=${event.reason}`);
    else console.log('[close] Connection died');
    
}

wfSocket.onerror = function (error) {
    alert(`[error] ${error}`);
}
