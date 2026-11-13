// The bridge contract of examples/life/cuda: the field stays in C++ and each call answers with one frame,
// [packed cells, generation, living cells, microseconds spent in C++].
const call = (name, ...args) => webFront.cppFunction(name)(...args);
const alive = (cells, side, x, y) => (cells[(y * side + x) >> 3] & (1 << ((y * side + x) & 7))) !== 0;
const countBits = cells => cells.reduce((count, byte) => { for (; byte; byte >>>= 1) count += byte & 1; return count; }, 0);
const cpu = 0, cuda = 1, soup = 0, gliders = 1;

describe('Life CUDA example bridge', () => {
    let side, cudaAvailable, device;
    beforeAll(async () => {
        await webFront.ready;
        [side, cudaAvailable, device] = await call('life.cuda.info');
    });

    it('describes the field and the CUDA device', () => {
        expect(side).toBe(2048);
        expect(typeof cudaAvailable).toBe('boolean');
        expect(device !== '').toBe(cudaAvailable);
    });

    it('answers a seed with generation zero as a packed Uint8Array', async () => {
        const [cells, generation, population, microseconds] = await call('life.cuda.seed', gliders, 0);
        expect(cells).toBeInstanceOf(Uint8Array);
        expect(cells.length).toBe(side * side / 8);
        expect(generation).toBe(0);
        expect(microseconds).toBe(0);
        expect(population).toBe(5 * (side / 16) ** 2);
        expect(countBits(cells)).toBe(population);
        // The glider stamped at (6, 6): its top cell, and the dead cell left of it.
        expect(alive(cells, side, 7, 6)).toBeTrue();
        expect(alive(cells, side, 6, 6)).toBeFalse();
    });

    it('advances the C++ field by the requested generations and returns the current frame for zero', async () => {
        await call('life.cuda.seed', gliders, 0);
        const [cells, generation, population, microseconds] = await call('life.cuda.advance', cpu, 4);
        expect(generation).toBe(4);
        expect(microseconds).toBeGreaterThan(0);
        expect(population).toBe(5 * (side / 16) ** 2);
        // Four generations move every glider one cell down and right.
        expect(alive(cells, side, 8, 7)).toBeTrue();
        expect(alive(cells, side, 7, 6)).toBeFalse();
        const [same, sameGeneration] = await call('life.cuda.advance', cpu, 0);
        expect(sameGeneration).toBe(4);
        expect(same).toEqual(cells);
        const [, later] = await call('life.cuda.advance', cpu, 1024);
        expect(later).toBe(1028);
    });

    it('rejects an oversized request, an unknown backend and an unknown pattern without advancing', async () => {
        await call('life.cuda.seed', soup, 3);
        await expectAsync(call('life.cuda.advance', cpu, 1025)).toBeRejectedWithError(/1024 generations/);
        await expectAsync(call('life.cuda.advance', 2, 1)).toBeRejected();
        const [, generation] = await call('life.cuda.advance', cpu, 0);
        expect(generation).toBe(0);
        await expectAsync(call('life.cuda.seed', 99, 0)).toBeRejected();
    });

    it('computes the same generations on CUDA as on the CPU, or rejects when CUDA is unavailable', async () => {
        await call('life.cuda.seed', soup, 5);
        if (!cudaAvailable) {
            await expectAsync(call('life.cuda.advance', cuda, 1)).toBeRejectedWithError(/CUDA/);
            const [, generation] = await call('life.cuda.advance', cpu, 0);
            expect(generation).toBe(0);
            return;
        }
        const [onGpu, gpuGeneration, gpuPopulation] = await call('life.cuda.advance', cuda, 8);
        await call('life.cuda.seed', soup, 5);
        const [onCpu, cpuGeneration, cpuPopulation] = await call('life.cuda.advance', cpu, 8);
        expect(gpuGeneration).toBe(cpuGeneration);
        expect(gpuPopulation).toBe(cpuPopulation);
        expect(onGpu).toEqual(onCpu);
    });
});
