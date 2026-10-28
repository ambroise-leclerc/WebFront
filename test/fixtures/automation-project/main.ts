const root = document.createElement('main');
root.innerHTML = '<button id="increment">Increment</button><output id="value">0</output><input id="name" type="text">';
document.body.append(root);
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
