// Application spaces (#671): a page loaded under /s/<name>/, here in a subdirectory, connects through the WebSocket that
// WebFront.js opens on the page's directory, and reaches the functions of its own space; the address of that WebSocket (#672).
async function spaceFrame(path) {
    const frame = document.createElement('iframe');
    const loaded = new Promise((resolve, reject) => { frame.onload = resolve; frame.onerror = reject; });
    frame.src = path;
    document.body.appendChild(frame);
    await loaded;
    await frame.contentWindow.webFront.ready;
    return frame;
}

describe('Application spaces', () => {
    it('connects a page of a subdirectory of a space to that space, and only to it', async () => {
        const alpha = await spaceFrame('/s/alpha/sub/space.html');
        const beta = await spaceFrame('/s/beta/sub/space.html');
        try {
            expect(await alpha.contentWindow.webFront.cppFunction('spaceName')()).toBe('alpha');
            expect(await beta.contentWindow.webFront.cppFunction('spaceName')()).toBe('beta');
            // The bridge connected from the page's directory, and C++ reads that path within the space (#672).
            expect(await alpha.contentWindow.webFront.cppFunction('bridgePath')()).toBe('/sub/');
            // This page is outside the spaces: the function of a space is not visible to it.
            await expectAsync(webFront.cppFunction('spaceName')()).toBeRejected();
        } finally {
            alpha.remove();
            beta.remove();
        }
    });
});

// The address of the bridge (#672): the directory of the page, so that the server, or a reverse proxy, tells which part of
// the site the page belongs to.
describe('The default address of the bridge', () => {
    const at = (protocol, host, pathname, search = '', hash = '') => ({ protocol, host, pathname, search, hash });

    it('is the root for a page at the root', () => {
        expect(WebFrontBridge.defaultUrl(at('http:', 'localhost:9002', '/'))).toBe('ws://localhost:9002/');
        expect(WebFrontBridge.defaultUrl(at('http:', 'localhost:9002', '/index.html'))).toBe('ws://localhost:9002/');
    });

    it('is the directory of the page, for a directory or a file under it', () => {
        expect(WebFrontBridge.defaultUrl(at('http:', 'example.org', '/a/'))).toBe('ws://example.org/a/');
        expect(WebFrontBridge.defaultUrl(at('http:', 'example.org', '/a/b/index.html'))).toBe('ws://example.org/a/b/');
        expect(WebFrontBridge.defaultUrl(at('http:', 'example.org', '/a/b'))).toBe('ws://example.org/a/');
    });

    it('leaves out the query and the fragment of the page', () => {
        expect(WebFrontBridge.defaultUrl(at('http:', 'example.org', '/a/b/index.html', '?x=1/2', '#c/d'))).toBe('ws://example.org/a/b/');
        expect(WebFrontBridge.defaultUrl(at('http:', 'example.org', '/a/', '?x=1', '#top'))).toBe('ws://example.org/a/');
    });

    it('is secure on a secure page, where a browser blocks ws:// as mixed content', () => {
        expect(WebFrontBridge.defaultUrl(at('https:', 'example.org', '/app/index.html'))).toBe('wss://example.org/app/');
    });

    it('is the directory of this very page, as a real location gives it', () => {
        const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
        const directory = location.pathname.slice(0, location.pathname.lastIndexOf('/') + 1);
        expect(WebFrontBridge.defaultUrl()).toBe(`${scheme}//${location.host}${directory}`);
        const page = new URL('/a/b/index.html?x=1#y', location.href);
        expect(WebFrontBridge.defaultUrl(page)).toBe(`${scheme}//${location.host}/a/b/`);
    });
});
