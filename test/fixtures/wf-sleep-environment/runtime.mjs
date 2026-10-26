// Minimal compatible environment for the driver's watchdog: no compiler or package installation.
// Other driver cases run real wf scripts; this one isolates a command already waiting in the browser.
// Make startup exceed the four-second command budget even on a fast host.
export async function mount() {
    await new Promise(resolve => setTimeout(resolve, 5000));
}

export async function runNonInteractive(argv, {bridge}) {
    if (argv.join(' ') !== 'run sleep') throw new Error('Expected the sleeping watchdog command');
    const sleeping = new Promise(resolve => setTimeout(resolve, 120000));
    const call = (name, ...args) => bridge.cppFunction(`webfront.developer.${name}`)(...args);
    const id = await call('begin', 'sleep.started', false);
    await call('write', id, 'output', 0, new TextEncoder().encode('running'));
    await call('commit', id);
    await sleeping;
    throw new Error('The watchdog did not stop the sleeping command');
}
