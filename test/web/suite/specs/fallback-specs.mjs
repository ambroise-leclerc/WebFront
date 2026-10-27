// Execute the actual fallback module in isolated documents with controlled bridge failures.
describe('Fallback readiness diagnostics', () => {
    let source, sourceUrl;
    beforeAll(async () => {
        const response = await fetch('/webfront-fallback.mjs');
        expect(response.ok).toBeTrue();
        source = await response.text();
        sourceUrl = response.url;
    });
    async function diagnostic(moduleSource, bridgeFailure, domFailure) {
        const frame = document.createElement('iframe');
        // Blob modules need fully qualified URLs; preserve the actual dynamic import and error handling.
        const executable = moduleSource.replace(/import\('([^']+)'\)/g, (_, path) =>
            `import(${JSON.stringify(new URL(path, sourceUrl).href)})`);
        const url = URL.createObjectURL(new Blob([executable], {type: 'text/javascript'}));
        const loaded = new Promise(resolve => { frame.onload = resolve; });
        frame.srcdoc = `<p id="status" data-state="connecting">Connecting…</p><div id="webfront-root"></div>
            <script>
            window.webFront = {
                ready: ${bridgeFailure ? "Promise.reject(new Error('bridge unavailable'))" : 'Promise.resolve()'},
                cppFunction: () => () => ${domFailure ? "Promise.reject(new Error('DOM API unavailable'))" : 'Promise.resolve()'}
            };
            webFront.ready.catch(() => {});
            <\/script><script type="module" src="${url}"><\/script>`;
        document.body.appendChild(frame);
        try {
            await loaded;
            const status = frame.contentDocument.getElementById('status');
            const deadline = performance.now() + 3000;
            while (!['error', 'dom-error'].includes(status.dataset.state)) {
                if (performance.now() > deadline) throw new Error(`Fallback did not report a failure: ${status.textContent}`);
                await new Promise(resolve => setTimeout(resolve, 5));
            }
            return {text: status.textContent, state: status.dataset.state};
        } finally { frame.remove(); URL.revokeObjectURL(url); }
    }
    for (const path of ['/embedded-fallback.html', '/nested-fallback/index.html']) {
        it(`loads the fallback and lays out its C++ root below the welcome text at ${path}`, async () => {
            const frame = document.createElement('iframe');
            const loaded = new Promise(resolve => { frame.onload = resolve; });
            frame.src = path;
            document.body.appendChild(frame);
            try {
                await loaded;
                const page = frame.contentWindow;
                await page.webFront.ready;
                await page.webFront.cppFunction('domDocumentBuild')();
                const root = page.document.getElementById('webfront-root');
                const deadline = performance.now() + 3000;
                while (!root.querySelector('input')) {
                    if (performance.now() > deadline) throw new Error('C++ root was not populated on the fallback page');
                    await new Promise(resolve => setTimeout(resolve, 5));
                }
                const main = page.document.querySelector('main');
                expect(root.getBoundingClientRect().top).toBeGreaterThanOrEqual(main.getBoundingClientRect().bottom);
                if (path.startsWith('/nested-fallback/')) {
                    const resources = page.performance.getEntriesByType('resource').map(entry => entry.name);
                    expect(resources).toContain(new URL('/nested-fallback/webfront-dom.mjs', location.href).href);
                }
            } finally { frame.remove(); }
        });
    }

    it('shows an import failure after a successful bridge handshake', async () => {
        const original = "import('./webfront-dom.mjs')";
        expect(source).toContain(original);
        const result = await diagnostic(source.replace(original, "import('/missing-dom-module-381.mjs')"), false, false);
        expect(result.state).toBe('dom-error');
        expect(result.text).toContain('Connected to WebFront; DOM unavailable:');
    });
    it('distinguishes an unavailable DOM API from a working bridge', async () => {
        const result = await diagnostic(source, false, true);
        expect(result.state).toBe('dom-error');
        expect(result.text).toBe('Connected to WebFront; DOM unavailable: DOM API unavailable');
    });
    it('keeps the initial connection failure diagnostic', async () => {
        const result = await diagnostic(source, true, false);
        expect(result.state).toBe('error');
        expect(result.text).toBe('Connection failed: bridge unavailable');
    });
});
