// A valid spec followed by a top-level throw: the file must not be silently half-loaded.
describe('Before the failure', () => { it('would pass', () => { expect(true).toBe(true); }); });
throw new Error('top-level failure');
