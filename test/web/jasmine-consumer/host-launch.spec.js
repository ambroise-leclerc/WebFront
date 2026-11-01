// An authorized program that cannot start is reported, and the spec asserting success fails.
describe('A spec whose program cannot start', () => {
    it('fails', async () => {
        const [notExecutable] = await webFrontHost.programs();
        const result = await webFrontHost.run({program: notExecutable});
        expect(result.status).toBe('exited');
    });
});
