import { mount } from '@webfront/pty';
import { Terminal } from '@xterm/xterm';
import { FitAddon } from '@xterm/addon-fit';

export async function attach(element: HTMLElement): Promise<void> {
    const page = await mount(element, {
        terminal: new Terminal(),
        fit: new FitAddon(),
        onExit: code => { element.dataset.exitCode = code.toString(); },
    });
    await page.hangUp();
    page.dispose();
}
