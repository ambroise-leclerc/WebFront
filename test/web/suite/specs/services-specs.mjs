// @webfront/services is resolved from the exact archive that wf installed from the local registry (#461): the skeleton
// proves that the package is installed, resolved by the bundler and runs in the page, with no resource added to the core.
import {services, selfTest} from '@webfront/services';

describe('Optional @webfront/services package', () => {
    it('is bundled from the installed archive and runs in the page', () => {
        expect(selfTest()).toBe('@webfront/services');
    });

    it('starts with no service, frozen', () => {
        expect(Object.keys(services)).toEqual([]);
        expect(Object.isFrozen(services)).toBeTrue();
    });
});
