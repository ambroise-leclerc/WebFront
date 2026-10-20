// Autonomous real-engine test: compiled WASM, npm graph, PI bridge, preview, cache and embedding.
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const assert = (condition, message) => { if (!condition) throw new Error(message); };
try {
    await webFront.ready;
    for (let attempt = 0; attempt < 300; ++attempt) {
        const text = document.getElementById('status')?.textContent ?? '';
        if (text.startsWith('Bootstrap failed:')) throw new Error(text);
        if (text.startsWith('Developer environment ready.')) break;
        if (attempt === 299) throw new Error('Bootstrap readiness timed out');
        await delay(100);
    }
    const runtime = await import('/webfront/developer/environment/runtime.mjs');
    const config = JSON.parse(await webFront.cppFunction('webfront.developer.config')());
    const environment = await (await fetch('/webfront/developer/environment/environment.json')).json();
    config.starters = Object.fromEntries(Object.entries(environment.starters).map(([name, value]) => [name, {url: new URL(value.archive, new URL(config.environment, location.href)).href, integrity: value.integrity}]));
    const context = {bridge: webFront, config, preview: document.getElementById('preview'), signal: new AbortController().signal,
        write: text => console.log(text)};
    await runtime.execute(['create', 'pi'], context);
    const overwriteRefused = await runtime.execute(['create', 'pi'], context).then(() => false, () => true);
    assert(overwriteRefused, 'A starter must not replace an existing project');
    await runtime.execute(['install', 'react@19.3.1', 'react-dom@19.3.1'], context);
    const lock = await runtime.readLock(webFront);
    assert(Object.keys(lock.packages).length === 3, 'React, React DOM and scheduler must be resolved');
    await runtime.execute(['preview', 'ui/main.tsx'], context);
    const frame = context.preview;
    for (let attempt = 0; attempt < 300; ++attempt) {
        if (frame.contentDocument?.querySelector('button')) break;
        if (attempt === 299) throw new Error('React preview did not mount');
        await delay(50);
    }
    frame.contentDocument.querySelector('button').click();
    for (let attempt = 0; attempt < 100; ++attempt) {
        if (frame.contentDocument.querySelector('output')?.textContent.startsWith('3.14159265358979323846264338327950288419716939937510')) break;
        if (attempt === 99) throw new Error('React preview did not display C++ PI');
        await delay(50);
    }
    await runtime.execute(['embed', 'ui/main.tsx', '--output', 'generated/PiUI.hpp'], context);
    // Repeat the browser build to prove reproducibility, including licenses and packed gzip bytes.
    const first = await webFront.cppFunction('test.header')();
    await runtime.execute(['embed', 'ui/main.tsx', '--output', 'generated/PiUI.hpp'], context);
    assert(await webFront.cppFunction('test.header')() === first, 'Header generation must be deterministic');
    const failed = await runtime.execute(['install', 'INVALID-NPM-NAME'], context).then(() => false, () => true);
    assert(failed, 'Missing packages must reject');
    assert(JSON.stringify(await runtime.readLock(webFront)) === JSON.stringify(lock), 'A failed install must preserve the lock');
    await webFront.cppFunction('test.report')(true, 'WASM + npm + TSX + PI + preview + deterministic header passed');
} catch (error) {
    console.error(error);
    await webFront.cppFunction('test.report')(false, String(error.stack ?? error));
} finally {
    setTimeout(() => globalThis.close(), 0);
}
