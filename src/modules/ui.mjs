import { importedModuleNote } from './messages.mjs';

function createElement(tagName, options = {}) {
    const element = document.createElement(tagName);
    if (options.className) {
        element.className = options.className;
    }
    if (options.textContent) {
        element.textContent = options.textContent;
    }
    return element;
}

export function renderDemo(root, onSend) {
    const panel = createElement('main', { className: 'panel' });
    const title = createElement('h1', { textContent: 'ES modules served by WebFront' });
    const description = createElement('p', {
        textContent: 'This page is bootstrapped by a single entry module that imports other .mjs files using relative paths.'
    });
    const moduleNote = createElement('p', {
        className: 'module-note',
        textContent: importedModuleNote()
    });
    const controls = createElement('div', { className: 'stack' });
    const inputLabel = createElement('label', { textContent: 'Message for C++' });
    const input = document.createElement('input');
    input.id = 'cpp-message';
    inputLabel.htmlFor = input.id;
    input.placeholder = 'Type a message for C++';
    input.value = 'Hello from ES modules';
    const button = createElement('button', { textContent: 'Send to C++' });
    const status = createElement('div', { className: 'status', textContent: 'Waiting for C++…' });

    button.addEventListener('click', () => {
        status.textContent = onSend(input.value.trim());
    });

    controls.append(inputLabel, input, button, status);
    panel.append(title, description, moduleNote, controls);
    root.replaceChildren(panel);

    return {
        showFromCpp(message) {
            status.textContent = `Received from C++: ${message}`;
        }
    };
}
