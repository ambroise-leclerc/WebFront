// Application spaces (#671): a page loaded under /s/<name>/, here in a subdirectory, connects through the WebSocket that
// WebFront.js opens on the page's directory, and reaches the functions of its own space.
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
            // This page is outside the spaces: the function of a space is not visible to it.
            await expectAsync(webFront.cppFunction('spaceName')()).toBeRejected();
        } finally {
            alpha.remove();
            beta.remove();
        }
    });
});
