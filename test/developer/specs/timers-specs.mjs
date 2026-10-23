import {withTimers, untilReady} from '../timers.mjs';
import {rejects, throws} from '../support.mjs';

describe('Scoped browser deadlines', () => {
    it('advances selected deadlines, preserves event-loop turns and restores globals on failure', async () => {
        const original = [setTimeout, clearTimeout, Date.now];
        await rejects(withTimers([2000], async clock => {
            let fired = false;
            const cancelled = setTimeout(() => fail('cancelled timer fired'), 2000);
            clearTimeout(cancelled);
            setTimeout(() => { fired = true; }, 2000);
            const before = Date.now();
            await new Promise(resolve => setTimeout(resolve, 0));
            expect(Date.now()).toBe(before);
            expect(fired).toBeFalse();
            clock.fire(2000);
            expect(fired).toBeTrue();
            throws(() => clock.fire(2000), /No pending/);
            throw new Error('scope failed');
        }), /scope failed/);
        expect([setTimeout, clearTimeout, Date.now]).toEqual(original);
    });
    it('freezes arbitrary deadlines and keeps real fallbacks inside and after the scope', async () => {
        await withTimers([1500, 3000, 20], async clock => {
            const now = Date.now();
            setTimeout(() => {}, 1500);
            setTimeout(() => {}, 3000);
            await new Promise(resolve => setTimeout(resolve, 20));
            expect(Date.now()).toBe(now);
            expect(clock.pending()).toBe(2);
            clock.fire(1500);
            clock.fire(3000);
        });
        let afterScope, cancelled = false;
        const completed = new Promise(resolve => { afterScope = resolve; });
        let cancelledId;
        await withTimers([20], async () => {
            setTimeout(afterScope, 20);
            cancelledId = setTimeout(() => { cancelled = true; }, 20);
        });
        clearTimeout(cancelledId);
        await completed;
        expect(cancelled).toBeFalse();
        await rejects(untilReady(new Promise(() => {}), Promise.resolve({error: new Error('deadline elapsed')})), /ended before readiness: deadline elapsed/);
        await untilReady(Promise.resolve(), new Promise(() => {}));
    });

});
