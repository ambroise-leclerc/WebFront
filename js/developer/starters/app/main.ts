// The interface of a WebFront application: TypeScript in ui/, compiled by `wf dev ui/main.ts`. The logic stays in C++.
import './style.css';

// The bridge to the C++ application. A call always returns a Promise: await it or handle its rejection.
declare const webFront: {ready: Promise<void>; cppFunction(name: string): (...values: unknown[]) => Promise<unknown>};

function element<K extends keyof HTMLElementTagNameMap>(tag: K, id: string, text = '') {
    const node = document.createElement(tag);
    node.id = id;  // explicit ids let `inspect`, `click` and `fill` (webfront-agent) and your specs name the elements
    node.textContent = text;
    return node;
}

const name = element('input', 'name');
name.value = 'WebFront';
const greet = element('button', 'greet', 'Call C++');
const answer = element('p', 'answer', 'Edit ui/main.ts: wf dev rebuilds this page when you save.');
document.getElementById('root')!.append(element('h1', 'title', 'Your application'), name, greet, answer);

greet.addEventListener('click', async () => {
    try {
        await webFront.ready;
        // Register it in C++: app.cppFunction("app.greet", [](std::string who) { return "Hello, " + who; });
        answer.textContent = String(await webFront.cppFunction('app.greet')(name.value));
    } catch (error) {
        answer.textContent = `app.greet failed: ${error instanceof Error ? error.message : error}. Register it with app.cppFunction("app.greet", ...) in C++.`;
    }
});
