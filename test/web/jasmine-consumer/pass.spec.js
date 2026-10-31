describe('The consumer application in its real page', () => {
    it('keeps its document, application script and callbacks', async () => {
        expect(document.title).toBe('Consumer application');
        expect(window.consumerLoaded).toBeTrue();
        document.getElementById('increment').click();
        expect(document.getElementById('value').textContent).toBe('1');
        expect(await webFront.cppFunction('consumer.answer')()).toBe(42);
    });
});
