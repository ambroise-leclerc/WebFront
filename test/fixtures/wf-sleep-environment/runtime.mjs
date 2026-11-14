// Minimal compatible environment for the driver's watchdog: no compiler or package installation.
// Other driver cases run real wf scripts; this one isolates a command already waiting in the browser.
// Make startup exceed the four-second command budget even on a fast host.
export async function mount() {
    await new Promise(resolve => setTimeout(resolve, 5000));
}

// `run sleep` waits on a timer; `run block` blocks the page, so that nothing in it can close the window.
export async function runNonInteractive(argv, {bridge}) {
    const command = argv.join(' ');
    if (command !== 'run sleep' && command !== 'run block') throw new Error('Expected the sleeping or blocking watchdog command');
    const name = command === 'run sleep' ? 'sleep' : 'block';
    const call = (operation, ...args) => bridge.cppFunction(`webfront.developer.${operation}`)(...args);
    const id = await call('begin', `${name}.started`, false);
    await call('write', id, 'output', 0, new TextEncoder().encode('running'));
    await call('commit', id);
    if (name === 'block')
        for (;;) {}
    await new Promise(resolve => setTimeout(resolve, 120000));
    throw new Error('The watchdog did not stop the sleeping command');
}
