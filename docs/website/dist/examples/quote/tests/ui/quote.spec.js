// tests/ui/quote.spec.js — classic JavaScript, loaded into the real page.
describe('quotation journey', () => {
    beforeAll(async () => { await window.appReady; });
    const element = id => document.getElementById(id);
    function until(predicate) {
        if (predicate()) return Promise.resolve();
        return new Promise((resolve, reject) => {
            const observer = new MutationObserver(() => {
                if (predicate()) { cleanup(); resolve(); }
            });
            const timer = setTimeout(() => {
                cleanup(); reject(new Error('UI did not settle within 2 seconds'));
            }, 2000);
            function cleanup() { clearTimeout(timer); observer.disconnect(); }
            observer.observe(document.body, {
                subtree: true, childList: true, characterData: true, attributes: true
            });
        });
    }
    it('enters a quantity and displays the C++ price', async () => {
        element('quantity').value = '3';
        element('quantity').dispatchEvent(new Event('input', {bubbles: true}));
        element('calculate').click();
        await until(() => !element('calculate').disabled);
        expect(element('status').textContent).toBe('Ready');
        expect(element('total').textContent).toBe('75');
    });
    it('shows an error for an invalid quantity', async () => {
        element('quantity').value = '0';
        element('calculate').click();
        await until(() => !element('calculate').disabled);
        expect(element('status').textContent).toContain('Error:');
        expect(element('total').textContent).toBe('');
    });
});
