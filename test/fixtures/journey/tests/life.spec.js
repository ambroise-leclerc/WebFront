describe('the Life starter project', () => {
    it('runs its specs in a real browser', () => {
        const canvas = document.createElement('canvas');
        expect(canvas.getContext('2d')).not.toBeNull();
    });
    it('packs 32 by 32 cells into 128 bytes, least significant bit first', () => {
        const cells = new Uint8Array(32 * 32 / 8);
        for (const i of [330, 331, 332]) cells[i >> 3] |= 1 << (i & 7);
        expect(cells.length).toBe(128);
        expect(cells[41]).toBe(0b00011100);
    });
});
