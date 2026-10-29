// Every spec is pending or excluded: nothing executed, so the suite must fail.
describe('Disabled specs', () => {
    xit('is skipped', () => {});
    it('is pending', () => { pending('not run'); });
});
