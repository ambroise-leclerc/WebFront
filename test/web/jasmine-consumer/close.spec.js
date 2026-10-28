// The window closes while a spec is running: no result can be reported, and that must fail even if the application
// itself exits successfully.
describe('A window closed mid-run', () => {
    it('never reaches jasmineDone', async () => {
        window.close();
        await new Promise(resolve => setTimeout(resolve, 4000));
    });
});
