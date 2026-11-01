// Without PROGRAMS in webfront_add_jasmine_test, the page has no native capability, whatever it asks.
describe('A test the owner granted nothing', () => {
    it('cannot run a program or create a temporary space', async () => {
        await expectAsync(webFrontHost.run({program: '/bin/sh', args: ['-c', 'true']})).toBeRejectedWithError(/No native capability/);
        await expectAsync(webFrontHost.makeTemp()).toBeRejectedWithError(/No native capability/);
        await expectAsync(webFrontHost.programs()).toBeRejected();
    });
});
