// A program that hits its timeout is a failing result like any other: the assertion fails the test.
describe('A spec whose program times out', () => {
    it('fails', async () => {
        const [probe] = await webFrontHost.programs();
        const result = await webFrontHost.run({program: probe, args: ['hang'], timeoutMs: 300});
        expect(result.status).toBe('exited');
    });
});
