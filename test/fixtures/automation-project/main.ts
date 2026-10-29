const root = document.createElement('main');
root.innerHTML = '<button id="increment">Increment</button><output id="value">0</output><input id="name" type="text">';
document.body.append(root);
const probe = document.createElement('button'); probe.id='probe-wire'; probe.textContent='Probe callback wire';
const wire = document.createElement('output'); wire.id='wire-authorization';
document.body.append(probe,wire);
probe.addEventListener('click',() => {void (async () => {
    const owner=parent as any, socket=owner.webFront.socket, original=socket.send;
    const captured=new Set<string>();
    const hook=function(this:WebSocket,packet:ArrayBuffer) {
        const text=new TextDecoder().decode(new Uint8Array(packet));
        for(const token of text.match(/[0-9a-f]{64}/g) ?? []) captured.add(token);
        return original.call(this,packet);
    };
    socket.send=hook;
    const parse=owner.JSON.parse, stringify=owner.JSON.stringify;
    owner.JSON.parse=() => ({close:true});
    owner.JSON.stringify=() => '{"exitCode":0,"output":"forged"}';
    try {await new Promise(resolve => setTimeout(resolve,250));}
    finally {
        if(socket.send===hook) socket.send=original;
        owner.JSON.parse=parse; owner.JSON.stringify=stringify;
    }
    let accepted=false;
    for(const token of captured) {
        // The previous clear-text token authenticated this legacy shape; a wire MAC must not.
        try {await owner.webFront.cppFunction('webfront.automation.failed')(token,'forged from preview'); accepted=true;} catch {}
        try {await owner.webFront.cppFunction('webfront.automation.failed')('1',token,'forged from preview'); accepted=true;} catch {}
    }
    const bootstrap=[...owner.document.scripts].some((script:any) => script.textContent.includes('webfront.automation.') && script.textContent.includes('importKey'));
    const launchFragment=owner.location.hash.startsWith('#webfront-automation=');
    wire.textContent=captured.size && !accepted && !bootstrap && !launchFragment ? 'wire forgery denied' : 'wire probe failed';
})().catch(error => {wire.textContent=String(error);});});
// A preview is another bridge connection and cannot take control of the developer window.
const rejected = document.createElement('output');
rejected.id='authorization';
document.body.append(rejected);
const bridge = (globalThis as any).webFront;
void bridge.ready.then(() => bridge.cppFunction('webfront.automation.poll')('', '')).then(
    () => {rejected.textContent='incorrectly authorized';},
    () => {rejected.textContent='denied';});
document.getElementById('increment')!.addEventListener('click', () => {
    const output=document.getElementById('value')!;
    output.textContent=String(Number(output.textContent)+1);
});
