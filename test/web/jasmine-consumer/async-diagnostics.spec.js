describe('Async spec diagnostics', () => {
    beforeEach(() => jasmine.clock().install());
    afterEach(() => jasmine.clock().uninstall());

    it('names a missing event and snapshots the state before removing listeners', async () => {
        const target = new EventTarget();
        spyOn(target, 'removeEventListener').and.callThrough();
        const waiting = webFrontTestAsync.steps(100, () => 'readyState=1, currentTime=10')
            .event(target, 'seeked', () => {});
        const checked = expectAsync(waiting).toBeRejectedWithError(/seeked: timed out after 100ms; readyState=1, currentTime=10/);
        jasmine.clock().tick(101);
        await checked;
        expect(target.removeEventListener).toHaveBeenCalledWith('seeked', jasmine.any(Function));
        expect(target.removeEventListener).toHaveBeenCalledWith('error', jasmine.any(Function));
    });

    it('bounds play even if timeupdate has already arrived', async () => {
        const target = new EventTarget();
        const waiting = webFrontTestAsync.steps(100).event(target, 'timeupdate', () => {
            target.dispatchEvent(new Event('timeupdate'));
            return new Promise(() => {});
        });
        const checked = expectAsync(waiting).toBeRejectedWithError(/timeupdate: timed out/);
        await Promise.resolve();
        await Promise.resolve();
        jasmine.clock().tick(101);
        await checked;
    });

    it('catches synchronous events and cleans up after success', async () => {
        const target = new EventTarget();
        spyOn(target, 'removeEventListener').and.callThrough();
        await webFrontTestAsync.steps(100).event(target, 'change', () => target.dispatchEvent(new Event('change')));
        expect(target.removeEventListener).toHaveBeenCalledTimes(2);
        target.dispatchEvent(new Event('error')); // A previous wait no longer observes this event.
    });

    it('reports media errors and play rejections with their stage', async () => {
        const target = new EventTarget();
        target.error = {code: 3, message: 'decode failed'};
        await expectAsync(webFrontTestAsync.steps(100).event(target, 'loadedmetadata', () => target.dispatchEvent(new Event('error'))))
            .toBeRejectedWithError(/loadedmetadata: error event \(3: decode failed\)/);
        await expectAsync(webFrontTestAsync.steps(100).event(target, 'timeupdate', () => Promise.reject(new Error('play denied'))))
            .toBeRejectedWithError('timeupdate: play denied');
    });

    it('ignores a queued seeked belonging to a different position', async () => {
        const target = new EventTarget();
        let position = 0;
        let completed = false;
        const waiting = webFrontTestAsync.steps(100).event(target, 'seeked', () => {}, 'seeked (middle)', () => position === 10)
            .then(() => { completed = true; });
        target.dispatchEvent(new Event('seeked'));
        await Promise.resolve();
        await Promise.resolve();
        expect(completed).toBeFalse();
        position = 10;
        target.dispatchEvent(new Event('seeked'));
        await waiting;
        expect(completed).toBeTrue();
    });

    it('names a stalled screenshot or bridge operation and observes late rejections', async () => {
        let reject;
        const waiting = webFrontTestAsync.steps(100).step('screenshot(file-input)', () => new Promise((_, fail) => { reject = fail; }));
        const checked = expectAsync(waiting).toBeRejectedWithError('screenshot(file-input): timed out after 100ms');
        await Promise.resolve();
        jasmine.clock().tick(101);
        await checked;
        reject(new Error('late native failure'));
        await Promise.resolve();
    });

    it('shares the deadline across steps and does not start work after expiry', async () => {
        let now = 0;
        spyOn(performance, 'now').and.callFake(() => now);
        const waits = webFrontTestAsync.steps(100);
        await waits.step('first', () => { now = 101; });
        const action = jasmine.createSpy('expired work');
        await expectAsync(waits.step('second', action)).toBeRejectedWithError('second: timed out after 100ms');
        expect(action).not.toHaveBeenCalled();
    });
});
