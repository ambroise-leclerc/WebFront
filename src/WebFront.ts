"use strict";
function greet(person : string, date : Date) {
    console.log('Hello ${person}, today is ${date.toDateString()}!')
}

greet("Brendan", new Date());

let wfSocket = new WebSocket("ws://localhost", "WebFront_0.1");
wfSocket.onopen = function (event) {
    console.log("wfSocket opened");
    wfSocket.send("Voici un text que le serveur attend de recevoir");
}

