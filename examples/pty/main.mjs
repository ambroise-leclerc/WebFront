import { Terminal } from '@xterm/xterm';
import { FitAddon } from '@xterm/addon-fit';
import '@xterm/xterm/css/xterm.css';
import { mount } from './terminal.mjs';

document.body.style.margin = '0';
const element = document.createElement('main');
element.style.height = 'calc(100dvh - 2rem)';
const close = document.createElement('button');
close.textContent = 'Hang up';
const status = document.createElement('output');
status.setAttribute('aria-live', 'polite');
document.body.replaceChildren(element, close, status);
close.disabled = true;

async function start() {
    try {
        const terminal = new Terminal({ cursorBlink: true });
        const session = await mount(element, {
            terminal, fit: new FitAddon(),
            onError: error => { status.textContent = error.message; },
            onExit: code => { close.disabled = true; status.textContent = ` Process exited (${code})`; },
            onTruncated: () => { status.textContent = ' Showing the latest terminal output'; },
        });
        close.disabled = false;
        close.onclick = async () => {
            close.disabled = true;
            try { await session.hangUp(); }
            catch (error) { status.textContent = error.message; }
        };
        terminal.focus();
    } catch (error) {
        status.textContent = error.message;
    }
}

void start();
