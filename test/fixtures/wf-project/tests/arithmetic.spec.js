describe('arithmetic', () => {
    it('adds', () => { expect(1 + 1).toBe(2); });
    it('fails on purpose', () => { expect(1 + 1).toBe(3); });
    // Never settles, within a Jasmine limit of an hour: the driver's limit ends the run (WfDriverTestStage).
    it('hangs on purpose', () => new Promise(() => {}), 3600000);
});
