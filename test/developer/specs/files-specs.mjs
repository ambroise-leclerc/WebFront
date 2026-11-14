// Specs of the terminal's file commands (files.mjs): ls, cat, tree and mounts read the project and nothing else. The
// commands as the terminal runs them are checked in test/web/developer-specs.mjs.
import {ls, cat, tree, mounts, projectPath, nature, columns, longListing, catLimit, treeLimit, withheld, withheldReason,
    fileCommandHelp} from '../../../js/developer/environment/files.mjs';
import {reservedNames} from '../../../js/developer/environment/commands.mjs';
import {memoryBridge} from '../memory-bridge.mjs';
import {rejects, throws} from '../support.mjs';

describe('The file commands of the terminal', () => {
    const encode = text => new TextEncoder().encode(text);
    const manifest = {
        entries: {main: 'ui/main.tsx'}, providers: {AppUI: {entry: 'main', output: 'generated/AppUI.hpp'}},
        scripts: {icons: 'tools/icons.ts --size 64', build: 'wf embed AppUI'}, test: {specs: ['ui/main-specs.ts']}};
    const project = () => memoryBridge(new Map([
        ['webfront.project.json', encode('{}')], ['webfront-packages.lock.json', encode('{}')],
        ['ui/main.tsx', encode('export {}\n')], ['ui/main-specs.ts', encode('')], ['ui/parts/a.ts', encode('a')],
        ['ui/parts/deep/b.ts', encode('b')], ['tools/icons.ts', encode('')], ['generated/AppUI.hpp', encode('#pragma once\n')],
        ['.hidden', encode('h')], ['.webfront/developer/packages/react/index.js', encode('')],
        ['.webfront/developer/preview/index.html', encode('<p>')]]));

    describe('paths', () => {
        it('are relative to the project, which is also "." and the empty path', () => {
            expect(projectPath()).toBe('');
            expect(projectPath('.')).toBe('');
            expect(projectPath('./ui/')).toBe('ui');
            expect(projectPath('ui/parts')).toBe('ui/parts');
        });

        it('never leave the project', () => {
            for (const path of ['/', '/etc/passwd', '..', 'ui/../..', 'ui//parts', 'C:\\Windows', 'ui/./parts'])
                throws(() => projectPath(path), /relative to the project directory/);
        });
    });

    describe('ls', () => {
        it('lists a directory in columns, without the hidden entries unless -a asks for them', async () => {
            const {bridge} = project();
            expect(await ls([], {bridge})).toBe('generated/                   webfront-packages.lock.json\ntools/                       webfront.project.json\nui/');
            expect((await ls([], {bridge, width: 1})).split('\n')).toEqual(['generated/', 'tools/', 'ui/', 'webfront-packages.lock.json', 'webfront.project.json']);
            expect((await ls(['-a'], {bridge, width: 1})).split('\n')).toEqual(['.hidden', '.webfront/', 'generated/', 'tools/', 'ui/', 'webfront-packages.lock.json', 'webfront.project.json']);
            expect((await ls(['ui/'], {bridge, width: 1})).split('\n')).toEqual(['main-specs.ts', 'main.tsx', 'parts/']);
        });

        it('names a file given as its path, and refuses what the project does not have', async () => {
            const {bridge} = project();
            expect(await ls(['ui/main.tsx'], {bridge})).toBe('main.tsx');
            expect(await ls(['.hidden'], {bridge})).toBe('.hidden');
            await rejects(ls(['ui/missing'], {bridge}), /ui\/missing: no such file or directory in the project/);
            await rejects(ls(['../outside'], {bridge}), /relative to the project directory/);
            await rejects(ls(['ui', 'tools'], {bridge}), /Usage: ls/);
            await rejects(ls(['-x'], {bridge}), /Usage: ls/);
        });

        it('describes entries with -l: type, size, and what wf knows of each one', async () => {
            const {bridge} = project();
            const lines = (await ls(['-la'], {bridge, manifest: async () => manifest})).split('\n');
            expect(lines.find(line => line.includes('webfront.project.json'))).toMatch(/^- +2  .*webfront\.project\.json  \[manifest\]$/);
            expect(lines.find(line => line.includes('lock.json'))).toMatch(/\[lock\]$/);
            expect(lines.find(line => line.includes('.webfront/'))).toMatch(/^d +-  .*\.webfront\/  \[managed by wf\]$/);
            expect(lines.find(line => line.endsWith('ui/'))).toMatch(/^d /);
            const ui = (await ls(['-l', 'ui'], {bridge, manifest: async () => manifest})).split('\n');
            expect(ui.find(line => line.includes('main.tsx'))).toMatch(/^- +10  .*main\.tsx  \[entry main\]$/);
            expect(ui.find(line => line.includes('main-specs.ts'))).toMatch(/\[spec\]$/);
            expect(await ls(['-l', 'generated/AppUI.hpp'], {bridge, manifest: async () => manifest})).toMatch(/AppUI\.hpp  \[generated provider AppUI\]$/);
        });

        it('lists with -l without a manifest, and says so when the application cannot describe entries', async () => {
            const {bridge} = project();
            expect(await ls(['-l', 'tools'], {bridge, manifest: async () => { throw new Error('unreadable'); }})).toMatch(/icons\.ts$/);
            const older = {cppFunction: name => name === 'webfront.developer.entries'
                ? async () => { throw new Error("C++ function 'webfront.developer.entries' was not found"); } : bridge.cppFunction(name)};
            await rejects(ls(['-l'], {bridge: older}), /needs a more recent WebFront library/);
            expect(await ls(['tools'], {bridge: older})).toBe('icons.ts');
        });

        it('shows a date for a known modification time', () => {
            const at = new Date(2026, 10, 14, 9, 5).getTime();
            expect(longListing([{name: 'a.ts', type: 'file', size: 12, modified: at}], '', null)).toBe('- 12  2026-11-14 09:05  a.ts');
        });
    });

    describe('nature', () => {
        it('names the manifest, the lock, what wf manages and what the manifest declares', () => {
            expect(nature('webfront.project.json')).toBe('manifest');
            expect(nature('webfront-packages.lock.json')).toBe('lock');
            expect(nature('.webfront/developer/packages/react')).toMatch(/^package cache/);
            expect(nature('.webfront/developer/preview')).toMatch(/served to the browser/);
            expect(nature('.webfront/developer/history.json')).toBe('terminal history');
            expect(nature('.webfront/other')).toBe('managed by wf');
            expect(nature('tools/icons.ts', manifest)).toBe('script icons');
            expect(nature('wf', manifest)).toBe('');
            expect(nature('ui/parts/a.ts', manifest)).toBe('');
            expect(nature('ui/main.tsx', {entries: {main: 'ui/main.tsx', other: 'ui/main.tsx'}})).toBe('entry main, entry other');
        });

        it('reads a manifest that is not valid without failing', () => {
            expect(nature('ui/main.tsx', {entries: 'ui/main.tsx', providers: [null], scripts: {a: 3}, test: {specs: 'ui/main.tsx'}})).toBe('');
        });
    });

    describe('columns', () => {
        it('are read downwards and fit the width', () => {
            expect(columns([])).toBe('');
            expect(columns(['a', 'b', 'c', 'd', 'e'], 6)).toBe('a  d\nb  e\nc');
            expect(columns(['long-name', 'b'], 4)).toBe('long-name\nb');
        });
    });

    describe('cat', () => {
        it('prints text files one after the other, each ending its line', async () => {
            const {bridge} = project();
            expect(await cat(['ui/main.tsx', 'ui/parts/a.ts'], {bridge})).toBe('export {}\na\n');
            expect(await cat(['ui/main-specs.ts'], {bridge})).toBe('');
        });

        it('shows control characters instead of sending them to the terminal', async () => {
            const {bridge} = memoryBridge(new Map([['a.txt', encode('one\r\ntwo\tx\x1b[2J\x7f')]]));
            expect(await cat(['a.txt'], {bridge})).toBe('one\ntwo\tx^[[2J^?\n');
        });

        it('refuses a binary file, a file that is not UTF-8, a large one, a directory and a missing file', async () => {
            const {bridge, storage} = project();
            storage.set('image.png', new Uint8Array([0x89, 0x50, 0, 1]));
            storage.set('latin1.txt', new Uint8Array([0x63, 0x61, 0x66, 0xe9]));
            storage.set('large.txt', new Uint8Array(catLimit + 1).fill(0x61));
            storage.set('limit.txt', new Uint8Array(catLimit).fill(0x61));
            await rejects(cat(['image.png'], {bridge}), /image\.png is not a text file/);
            await rejects(cat(['latin1.txt'], {bridge}), /latin1\.txt is not UTF-8 text/);
            await rejects(cat(['large.txt'], {bridge}), /large\.txt is larger than 1 MiB/);
            expect((await cat(['limit.txt'], {bridge})).length).toBe(catLimit + 1);
            await rejects(cat(['.'], {bridge}), /is a directory/);
            await rejects(cat(['ui/missing.ts'], {bridge}), /ui\/missing\.ts: no such file in the project/);
            await rejects(cat([], {bridge}), /Usage: cat/);
            await rejects(cat(['-n', 'ui/main.tsx'], {bridge}), /Usage: cat/);
            await rejects(cat(['../outside'], {bridge}), /relative to the project directory/);
        });

        it('stops when interrupted', async () => {
            const {bridge} = project();
            const interrupted = new AbortController();
            interrupted.abort(new Error('Interrupted'));
            await rejects(cat(['ui/main.tsx'], {bridge, signal: interrupted.signal}), /Interrupted/);
        });
    });

    describe('tree', () => {
        it('draws the visible project, three levels deep', async () => {
            const {bridge} = project();
            expect(await tree([], {bridge})).toBe([
                '.',
                '├── generated/',
                '│   └── AppUI.hpp',
                '├── tools/',
                '│   └── icons.ts',
                '├── ui/',
                '│   ├── main-specs.ts',
                '│   ├── main.tsx',
                '│   └── parts/',
                '│       ├── a.ts',
                '│       └── deep/',
                '├── webfront-packages.lock.json',
                '└── webfront.project.json',
                '',
                '5 directories, 7 files'].join('\n'));
        });

        it('follows -L and a path', async () => {
            const {bridge} = project();
            expect(await tree(['-L', '1', 'ui'], {bridge})).toBe('ui/\n├── main-specs.ts\n├── main.tsx\n└── parts/\n\n1 directory, 2 files');
            expect(await tree(['ui/parts', '-L', '4'], {bridge})).toContain('    └── b.ts');
            await rejects(tree(['-L'], {bridge}), /Usage: tree/);
            await rejects(tree(['-L', '0'], {bridge}), /depth from 1 to 32/);
            await rejects(tree(['-L', 'x'], {bridge}), /depth from 1 to 32/);
            await rejects(tree(['missing'], {bridge}), /missing: no such directory in the project/);
            await rejects(tree(['ui/main.tsx'], {bridge}), /no such directory/);
        });

        it('shows hidden entries with -a, and opens .webfront only when it is named', async () => {
            const {bridge} = project();
            const all = await tree(['-a'], {bridge});
            expect(all).toContain('├── .hidden');
            expect(all).toContain('├── .webfront/  [managed by wf: tree .webfront]');
            expect(all).not.toContain('developer/');
            expect(await tree(['.webfront', '-L', '2'], {bridge})).toContain('    ├── packages/');
        });

        it('stops after its limit of entries, and when interrupted', async () => {
            const storage = new Map();
            for (let index = 0; index < treeLimit + 5; ++index) storage.set(`many/${String(index).padStart(5, '0')}.ts`, encode(''));
            const {bridge} = memoryBridge(storage);
            const lines = (await tree([], {bridge})).split('\n');
            expect(lines).toContain(`... more than ${treeLimit} entries: name a subdirectory or lower -L`);
            expect(lines.length).toBeLessThan(treeLimit + 6);
            const interrupted = new AbortController();
            interrupted.abort(new Error('Interrupted'));
            await rejects(tree([], {bridge, signal: interrupted.signal}), /Interrupted/);
        });
    });

    describe('mounts', () => {
        it('describes what is served, what is published and what the commands read', async () => {
            const {bridge} = project();
            const text = await mounts([], {bridge, config: {environment: 'https://dist.example/environment.tgz', integrity: 'sha512-abc',
                registry: '/registry/', registryExplicit: true}});
            expect(text).toMatch(/\/WebFront\.js, .* embedded in the executable/);
            expect(text).toMatch(/\/webfront\/developer\/preview\/ +\.webfront\/developer\/preview\/ of the project \(published\)/);
            expect(text).toMatch(/\/webfront\/developer\/test\/ +\.webfront\/developer\/test\/ of the project \(empty\)/);
            expect(text).toContain('The sources of the project are never served');
            expect(text).toContain('manifest     webfront.project.json');
            expect(text).toContain('environment  https://dist.example/environment.tgz');
            expect(text).toContain('registry     /registry/ (chosen by the application)');
            await rejects(mounts(['x'], {bridge}), /Usage: mounts/);
        });

        it('works without a configuration', async () => {
            const {bridge} = project();
            expect(await mounts([], {bridge})).not.toContain('environment  ');
        });
    });

    describe('reserved names', () => {
        it('are wf, the commands of the terminal and the file commands it withholds', () => {
            expect([...reservedNames].sort()).toEqual(['wf', 'clear', 'exit', 'help', 'history', ...Object.keys(fileCommandHelp), ...withheld].sort());
            expect(new Set(reservedNames).size).toBe(reservedNames.length);
        });

        it('each say what to use instead', () => {
            for (const name of withheld) expect(withheldReason(name)).toMatch(new RegExp(`^${name} is not provided: `));
            expect(withheldReason('cd')).toContain('relative to the project');
            expect(withheldReason('rm')).toContain('edit the project in your editor');
            expect(withheldReason('grep')).toContain('ls, cat and tree');
        });
    });
});
