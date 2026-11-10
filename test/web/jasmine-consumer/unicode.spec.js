// These expected failures must reach C++ as valid JSON, including when the diagnostic is clipped (#504).
describe('Unicode reports 😀', () => {
    it('keeps a pair at the truncation boundary', () => fail('a'.repeat(2044) + '😀'));
    it('normalizes an isolated high surrogate', () => fail('high surrogate: \ud800'));
    it('normalizes an isolated low surrogate', () => fail('low surrogate: \udc00'));
    it('long-name-' + 'x'.repeat(3000) + '-end', () => fail('short diagnostic'));
});
it('\ufefftop-level name', () => fail('keeps a leading U+FEFF'));
