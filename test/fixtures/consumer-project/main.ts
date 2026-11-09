// The user interface of an application that registered its own `consumer.render` callback in C++.
const root = document.createElement('main');
root.innerHTML = '<button id="render">Render</button><output id="result">idle</output>';
document.body.append(root);
const bridge = (globalThis as any).webFront;
document.getElementById('render')!.addEventListener('click', () => {
    const output = document.getElementById('result')!;
    void bridge.ready.then(() => bridge.cppFunction('consumer.render')('hello')).then(
        (text: string) => {output.textContent = text;},
        (error: unknown) => {output.textContent = 'bridge error: ' + String(error);});
});
