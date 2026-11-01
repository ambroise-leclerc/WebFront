// The run of an unauthorized program is not caught: the test, and so CTest, must fail.
describe('A spec that runs an unauthorized program', () => {
    it('fails', async () => { await webFrontHost.run({program: '/bin/sh', args: ['-c', 'true']}); });
});
