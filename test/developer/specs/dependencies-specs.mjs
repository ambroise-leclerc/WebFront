// Specs of the dependency graph read from the lock (dependencies.mjs): wf list --tree, wf why and wf info.
import {dependencyTree, why, info, selectPackages, chainLimit} from '../../../js/developer/environment/dependencies.mjs';
import {throws} from '../support.mjs';

describe('The graph of the installed packages', () => {
    const node = (name, version, dependencies = {}, manifest = {}) =>
        ({name, version, integrity: `sha512-${name}`, key: `${name.replace('/', '+')}@${version}-key`, files: ['package.json', 'index.js'],
            dependencies, manifest: {name, version, ...manifest}});
    // react-dom needs scheduler and its peer react; @app/kit needs react-dom and an older scheduler.
    const lock = {
        format: 1,
        roots: {
            react: {requested: '^19.3.1', id: 'react@19.3.1'},
            'react-dom': {requested: '^19.3.1', id: 'react-dom@19.3.1'},
            '@app/kit': {requested: 'latest', id: '@app+kit@1.0.0'},
        },
        packages: {
            'react@19.3.1': node('react', '19.3.1', {}, {license: 'MIT', description: 'React'}),
            'react-dom@19.3.1': node('react-dom', '19.3.1', {scheduler: 'scheduler@0.28.0', react: 'react@19.3.1'},
                {license: 'MIT', dependencies: {scheduler: '^0.28.0'}, peerDependencies: {react: '^19.3.1'}}),
            'scheduler@0.28.0': node('scheduler', '0.28.0', {}, {license: {type: 'MIT'}}),
            'scheduler@0.20.0': node('scheduler', '0.20.0'),
            '@app+kit@1.0.0': node('@app/kit', '1.0.0', {'react-dom': 'react-dom@19.3.1', scheduler: 'scheduler@0.20.0'},
                {dependencies: {'react-dom': '19.x', scheduler: '~0.20.0'}, webfront: {bin: {make: 'make.mjs', check: 'check.mjs'}}}),
        },
    };

    describe('as a tree', () => {
        it('opens each package once under the roots, with the range that asks for it', () => {
            expect(dependencyTree(lock)).toBe([
                '@app/kit@1.0.0  (latest)',
                '├── react-dom@19.3.1  (19.x)',
                '│   ├── react@19.3.1  (peer ^19.3.1)',
                '│   └── scheduler@0.28.0  (^0.28.0)',
                '└── scheduler@0.20.0  (~0.20.0)',
                'react@19.3.1  (^19.3.1)',
                'react-dom@19.3.1  (^19.3.1)  deduped',
                '',
                '3 roots, 5 packages'].join('\n'));
        });

        it('says when nothing is installed, and ends on a cycle of peers', () => {
            expect(dependencyTree(null)).toBe('No installed packages.');
            expect(dependencyTree({roots: {}, packages: {}})).toBe('No installed packages.');
            const cycle = {roots: {a: {requested: '1', id: 'a@1'}}, packages: {'a@1': node('a', '1', {b: 'b@1'}), 'b@1': node('b', '1', {a: 'a@1'})}};
            expect(dependencyTree(cycle)).toBe('a@1  (1)\n└── b@1\n    └── a@1  deduped\n\n1 root, 2 packages');
        });
    });

    describe('why', () => {
        it('gives the chains from the roots to a package, for each of its versions', () => {
            expect(why(lock, 'scheduler')).toBe([
                'scheduler@0.20.0',
                '  @app/kit@1.0.0 (root, latest) > scheduler@0.20.0 (~0.20.0)',
                'scheduler@0.28.0',
                '  react-dom@19.3.1 (root, ^19.3.1) > scheduler@0.28.0 (^0.28.0)',
                '  @app/kit@1.0.0 (root, latest) > react-dom@19.3.1 (19.x) > scheduler@0.28.0 (^0.28.0)'].join('\n'));
            expect(why(lock, 'scheduler@0.20.0').split('\n').length).toBe(2);
        });

        it('names a root as one, with the packages that also need it', () => {
            expect(why(lock, 'react')).toBe([
                'react@19.3.1',
                '  react@19.3.1 (root, ^19.3.1)',
                '  react-dom@19.3.1 (root, ^19.3.1) > react@19.3.1 (peer ^19.3.1)',
                '  @app/kit@1.0.0 (root, latest) > react-dom@19.3.1 (19.x) > react@19.3.1 (peer ^19.3.1)'].join('\n'));
        });

        it('refuses a package that is not installed, ends on a cycle and bounds the chains', () => {
            throws(() => why(lock, 'vue'), /vue is not installed \(wf list names the installed packages\)/);
            throws(() => why(null, 'react'), /react is not installed/);
            const cycle = {roots: {a: {requested: '1', id: 'a@1'}}, packages: {'a@1': node('a', '1', {b: 'b@1'}), 'b@1': node('b', '1', {a: 'a@1'})}};
            expect(why(cycle, 'b')).toBe('b@1\n  a@1 (root, 1) > b@1');
            const orphan = {roots: {}, packages: {'a@1': node('a', '1')}};
            expect(why(orphan, 'a')).toContain('nothing requires it');
            // A diamond of n levels has 2^n chains to its last package.
            const packages = {'end@1': node('end', '1')}, roots = {};
            for (let level = 0; level < 6; ++level)
                for (const side of ['l', 'r']) {
                    const below = level === 5 ? {end: 'end@1'} : {[`l${level + 1}`]: `l${level + 1}@1`, [`r${level + 1}`]: `r${level + 1}@1`};
                    packages[`${side}${level}@1`] = node(`${side}${level}`, '1', below);
                }
            roots.l0 = {requested: '1', id: 'l0@1'};
            const lines = why({roots, packages}, 'end').split('\n');
            expect(lines.length).toBe(chainLimit + 2);
            expect(lines.at(-1)).toContain(`only the first ${chainLimit} chains`);
        });
    });

    describe('info', () => {
        it('describes a locked package: root, license, files, dependencies and dependents', () => {
            expect(info(lock, 'react-dom')).toBe([
                'react-dom@19.3.1',
                '  root          yes, requested ^19.3.1',
                '  license       MIT',
                '  integrity     sha512-react-dom',
                '  files         2 in .webfront/developer/packages/react-dom@19.3.1-key/',
                '  dependencies  react@19.3.1 (peer ^19.3.1), scheduler@0.28.0 (^0.28.0)',
                '  dependents    @app/kit@1.0.0'].join('\n'));
        });

        it('gives the description, the executables and every version of a name', () => {
            expect(info(lock, 'react')).toContain('  description   React');
            expect(info(lock, '@app/kit')).toContain('  executables   wf exec @app/kit:make, wf exec @app/kit:check');
            const versions = info(lock, 'scheduler');
            expect(versions).toContain('scheduler@0.20.0\n  root          no, installed for another package (wf why)\n  license       not declared');
            expect(versions).toContain('scheduler@0.28.0\n');
            expect(versions).toContain('  license       MIT');
            expect(selectPackages(lock, 'scheduler@0.28.0').length).toBe(1);
            throws(() => info(lock, 'scheduler@9.9.9'), /scheduler@9\.9\.9 is not installed/);
        });
    });
});
