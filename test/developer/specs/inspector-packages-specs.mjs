// Specs of the Packages panel of the inspectors (#683): the tree of the locked packages read from the lock through the
// bridge, with its peers, its packages already opened and the versions of a name; the search, the detail of a package with
// the chains of wf why, the read after a change of the lock, and the texts of packages shown as text. The graph itself is in
// dependencies-specs.mjs.
import {showPackagesPanel, searchPackages, readPackages, manifestDirectory, lockPeriod} from '../../../js/developer/environment/inspector-packages.mjs';
import {main} from '../../../js/developer/environment/inspector.mjs';
import {idOf, cacheKey} from '../../../js/developer/environment/registry.mjs';
import {dependenciesDigest, setManifestDirectory} from '../../../js/developer/environment/manifest.mjs';
import {memoryBridge} from '../memory-bridge.mjs';

const encode = text => new TextEncoder().encode(text);

/** A locked package as wf install records it. */
const node = (name, version, dependencies = {}, manifest = {}) => {
    const integrity = `sha512-${name.replace(/[^a-z]/g, '')}${version.replace(/\D/g, '')}`;
    return [idOf(name, version), {name, version, integrity, key: cacheKey(name, version, integrity), files: ['package.json', 'index.js'],
        dependencies, manifest: {name, version, ...manifest}}];
};

// react-dom needs scheduler and its peer react; @app/kit needs react-dom and an older scheduler.
const packages = () => Object.fromEntries([
    node('react', '19.3.1', {}, {license: 'MIT', description: 'React'}),
    node('react-dom', '19.3.1', {scheduler: 'scheduler@0.28.0', react: 'react@19.3.1'},
        {license: 'MIT', dependencies: {scheduler: '^0.28.0'}, peerDependencies: {react: '^19.3.1'}}),
    node('scheduler', '0.28.0', {}, {license: 'MIT'}),
    node('scheduler', '0.20.0'),
    node('@app/kit', '1.0.0', {'react-dom': 'react-dom@19.3.1', scheduler: 'scheduler@0.20.0'},
        {dependencies: {'react-dom': '19.x', scheduler: '~0.20.0'}, webfront: {bin: {make: 'make.mjs', check: 'check.mjs'}}}),
]);
const dependencies = {react: '^19.3.1', 'react-dom': '^19.3.1', '@app/kit': 'latest'};
const roots = () => ({
    react: {requested: '^19.3.1', id: 'react@19.3.1'},
    'react-dom': {requested: '^19.3.1', id: 'react-dom@19.3.1'},
    '@app/kit': {requested: 'latest', id: '@app+kit@1.0.0'},
});

/** Writes a manifest and its lock in the project, in `directory` when the application chose one. */
async function writeProject(storage, {wanted = dependencies, lockRoots = roots(), locked = packages(), directory = '', digest = wanted} = {}) {
    const prefix = directory ? `${directory}/` : '';
    storage.set(`${prefix}webfront.project.json`, encode(JSON.stringify({format: 1, name: 'app', version: '0.1.0', dependencies: wanted})));
    storage.set(`${prefix}webfront-packages.lock.json`,
        encode(JSON.stringify({format: 1, manifestDigest: await dependenciesDigest(digest), roots: lockRoots, packages: locked})));
}

/** Timers that run only when the spec says so. */
function manualTimers() {
    let pending = null, next = 1;
    const delays = [];
    return {
        delays,
        get pending() { return pending !== null; },
        setTimeout(run, delay) {
            delays.push(delay);
            pending = {id: next, run};
            return next++;
        },
        clearTimeout(id) { if (pending?.id === id) pending = null; },
        fire() {
            const {run} = pending;
            pending = null;
            run();
        },
    };
}

/** The memory project's bridge, which also counts the calls by function. */
function countingBridge(storage, extra = {}) {
    const {bridge} = memoryBridge(storage);
    const calls = [];
    return {calls, cppFunction: name => async (...args) => {
        calls.push(name);
        return extra[name] ? extra[name](...args) : bridge.cppFunction(name)(...args);
    }};
}

async function setup({storage = new Map(), write = true, extra = {}, ...project} = {}) {
    if (write) await writeProject(storage, project);
    const timers = manualTimers(), listeners = new Map();
    const view = {setTimeout: timers.setTimeout, clearTimeout: timers.clearTimeout, addEventListener: (name, handler) => listeners.set(name, handler)};
    const body = document.createElement('main');
    const status = document.createElement('p');
    body.append(status);
    document.body.append(body);
    const bridge = countingBridge(storage, extra);
    const panel = await showPackagesPanel({page: document, view, body, status, bridge});
    return {storage, timers, view, listeners, body, status, bridge, panel};
}

/** The tree as lines: the depth of each item, its name, its range and its badges. */
const lines = tree => [...tree.querySelectorAll('[role=treeitem]')].map(item => {
    const row = item.querySelector(':scope > .row');
    return [Number(item.getAttribute('aria-level')), row.querySelector('.package-name').textContent, row.querySelector('.package-range')?.textContent ?? '',
        [...row.querySelectorAll('.badge')].map(badge => badge.textContent).join(' ')].join('|');
});
const item = (tree, label, index = 0) => [...tree.querySelectorAll('[role=treeitem]')]
    .filter(candidate => candidate.querySelector(':scope > .row .package-name').textContent === label)[index];
const key = (target, name) => target.dispatchEvent(new KeyboardEvent('keydown', {key: name, bubbles: true, cancelable: true}));
const rowsOf = detail => Object.fromEntries([...detail.querySelectorAll('dt')].map(term => [term.textContent, term.nextElementSibling.textContent]));

describe('The Packages panel of the inspectors', () => {
    const shown = [];
    const open = async options => {
        const made = await setup(options);
        shown.push(made);
        return made;
    };
    afterEach(() => {
        for (const {panel, body} of shown.splice(0)) {
            panel.stop();
            body.remove();
        }
        setManifestDirectory('');
    });

    it('shows the locked packages under the roots of the manifest, with the peers, the packages already opened and the versions of a name', async () => {
        const {panel, status} = await open();
        expect(lines(panel.tree)).toEqual([
            '1|@app/kit@1.0.0|latest|',
            '2|react-dom@19.3.1|19.x|',
            '3|react@19.3.1|^19.3.1|peer',
            '3|scheduler@0.28.0|^0.28.0|2 versions',
            '2|scheduler@0.20.0|~0.20.0|2 versions',
            '1|react@19.3.1|^19.3.1|',
            '1|react-dom@19.3.1|^19.3.1|deduped',
        ]);
        expect(panel.summary.textContent).toBe('3 roots, 5 packages');
        expect(status.textContent).toBe('');
        expect(panel.tree.getAttribute('role')).toBe('tree');
        expect(panel.detail.hidden).toBeTrue();
        // One item takes the Tab key; the packages already opened have nothing to unfold.
        expect([...panel.tree.querySelectorAll('[tabindex="0"]')].length).toBe(1);
        expect(item(panel.tree, 'react-dom@19.3.1', 1).hasAttribute('aria-expanded')).toBeFalse();
        expect(item(panel.tree, 'react-dom@19.3.1', 0).getAttribute('aria-expanded')).toBe('true');
    });

    it('reads the lock alone, through the reads of the project, and writes nothing', async () => {
        const {bridge, storage} = await open();
        expect([...new Set(bridge.calls)].sort()).toEqual(['webfront.developer.config', 'webfront.developer.read', 'webfront.developer.state']);
        expect([...storage.keys()].sort()).toEqual(['webfront-packages.lock.json', 'webfront.project.json']);
    });

    it('shows the detail of a package: what wf info says, and the chains of wf why whose packages open in a click', async () => {
        const {panel} = await open();
        item(panel.tree, 'scheduler@0.28.0').querySelector('.package-name').click();
        expect(panel.selected).toBe('scheduler@0.28.0');
        expect(panel.detail.hidden).toBeFalse();
        expect(panel.detail.querySelector('h2').textContent).toBe('scheduler@0.28.0');
        expect(rowsOf(panel.detail)).toEqual({
            Root: 'no, installed for another package', License: 'MIT', Integrity: 'sha512-scheduler0280',
            Directory: '.webfront/developer/packages/scheduler@0.28.0-scheduler0280/', Dependencies: 'none', Dependents: 'react-dom@19.3.1'});
        expect([...panel.detail.querySelectorAll('ol.chains li')].map(chain => chain.textContent)).toEqual([
            'react-dom@19.3.1 (root, ^19.3.1) > scheduler@0.28.0 (^0.28.0)',
            '@app/kit@1.0.0 (root, latest) > react-dom@19.3.1 (19.x) > scheduler@0.28.0 (^0.28.0)']);
        expect(panel.detail.querySelector('details summary').textContent).toBe('2 files');
        expect([...panel.detail.querySelectorAll('ul.files li')].map(file => file.textContent)).toEqual(['package.json', 'index.js']);
        expect(item(panel.tree, 'scheduler@0.28.0').getAttribute('aria-selected')).toBe('true');

        // A package of a chain: its detail, with its executables, its dependencies and the ranges that ask for them.
        const followed = [...panel.detail.querySelectorAll('ol.chains button')].find(button => button.textContent === '@app/kit@1.0.0');
        followed.focus();
        followed.click();
        expect(panel.selected).toBe('@app+kit@1.0.0');
        // The link was replaced with the detail: the focus is on the title of the package it led to, not lost to the page.
        expect(document.activeElement).toBe(panel.detail.querySelector('h2'));
        expect(document.activeElement.textContent).toBe('@app/kit@1.0.0');
        const kit = rowsOf(panel.detail);
        expect(kit.Root).toBe('yes, requested latest');
        expect(kit.License).toBe('not declared');
        expect(kit.Dependencies).toBe('react-dom@19.3.1 (19.x), scheduler@0.20.0 (~0.20.0)');
        expect(kit.Executables).toBe('wf exec @app/kit:make, wf exec @app/kit:check');
        expect(panel.detail.querySelector('ol.chains').textContent).toBe('@app/kit@1.0.0 (root, latest)');
        // The description of a package, and a package that is not locked.
        expect(panel.select('react@19.3.1')).toBeTrue();
        expect(panel.detail.querySelector('p.description').textContent).toBe('React');
        expect(panel.select('vue@3.0.0')).toBeFalse();
        expect(panel.selected).toBe('react@19.3.1');
    });

    it('searches the packages by name, with every version of a name', async () => {
        const {panel} = await open();
        const type = text => {
            panel.search.value = text;
            panel.search.dispatchEvent(new Event('input'));
        };
        type('SCHED');
        expect(panel.tree.hidden).toBeTrue();
        expect(panel.matches.hidden).toBeFalse();
        expect([...panel.matches.children].map(match => match.textContent)).toEqual(['scheduler@0.20.02 versions', 'scheduler@0.28.02 versions']);
        type('react');
        expect([...panel.matches.querySelectorAll('button')].map(button => button.textContent)).toEqual(['react@19.3.1', 'react-dom@19.3.1']);
        expect([...panel.matches.children].map(match => match.querySelector('.badge.root') !== null)).toEqual([true, true]);
        panel.matches.querySelectorAll('button')[1].click();
        expect(panel.selected).toBe('react-dom@19.3.1');
        expect(panel.matches.querySelectorAll('button')[1].getAttribute('aria-pressed')).toBe('true');
        type('vue');
        expect(panel.matches.textContent).toBe('No locked package has this name.');
        // Without a query the tree comes back, the package chosen selected where its dependencies are listed.
        type('  ');
        expect(panel.tree.hidden).toBeFalse();
        expect(panel.matches.hidden).toBeTrue();
        expect(item(panel.tree, 'react-dom@19.3.1', 0).getAttribute('aria-selected')).toBe('true');
        expect(item(panel.tree, 'react-dom@19.3.1', 0).tabIndex).toBe(0);
        expect(searchPackages(null, 'a')).toEqual([]);
        expect(searchPackages({roots: roots(), packages: packages()}, '').length).toBe(5);
    });

    it('is walked with the keyboard: arrows, Home and End, folding, and from a package already opened to where it is opened', async () => {
        const {panel} = await open();
        const focused = () => document.activeElement.querySelector(':scope > .row .package-name')?.textContent;
        const kit = item(panel.tree, '@app/kit@1.0.0');
        kit.focus();
        key(kit, 'ArrowDown');
        expect(focused()).toBe('react-dom@19.3.1');
        expect(document.activeElement.tabIndex).toBe(0);
        expect(kit.tabIndex).toBe(-1);
        key(document.activeElement, 'ArrowLeft');   // folds react-dom
        expect(document.activeElement.getAttribute('aria-expanded')).toBe('false');
        key(document.activeElement, 'ArrowDown');   // its dependencies are skipped
        expect(focused()).toBe('scheduler@0.20.0');
        key(document.activeElement, 'ArrowLeft');   // a leaf: to its parent
        expect(focused()).toBe('@app/kit@1.0.0');
        key(document.activeElement, 'End');
        expect(document.activeElement).toBe(item(panel.tree, 'react-dom@19.3.1', 1));
        key(document.activeElement, 'ArrowRight');  // deduped: to the place where it is opened
        expect(document.activeElement).toBe(item(panel.tree, 'react-dom@19.3.1', 0));
        key(document.activeElement, 'ArrowRight');  // unfolds it
        expect(document.activeElement.getAttribute('aria-expanded')).toBe('true');
        key(document.activeElement, 'ArrowRight');  // then goes to its first dependency
        expect(focused()).toBe('react@19.3.1');
        key(document.activeElement, 'Enter');
        expect(panel.selected).toBe('react@19.3.1');
        key(document.activeElement, 'Home');
        expect(focused()).toBe('@app/kit@1.0.0');
        key(document.activeElement, 'ArrowUp');     // nothing above the first item
        expect(focused()).toBe('@app/kit@1.0.0');
        // The mouse folds with the marker, without selecting.
        kit.querySelector('.twisty').click();
        expect(kit.getAttribute('aria-expanded')).toBe('false');
        expect(panel.selected).toBe('react@19.3.1');
    });

    it('reads the lock again when it changes, and only then; the folded packages and the selection stay', async () => {
        const {panel, storage, timers, bridge, listeners} = await open();
        expect(timers.delays).toEqual([lockPeriod]);
        panel.select('scheduler@0.20.0');
        key(item(panel.tree, 'react-dom@19.3.1'), 'ArrowLeft');
        const reads = () => bridge.calls.filter(name => name === 'webfront.developer.read').length;
        const before = reads();
        timers.fire();
        await panel.settled();
        expect(reads()).withContext('an unchanged lock is not read again').toBe(before);
        expect(timers.delays).toEqual([lockPeriod, lockPeriod]);

        // wf install adds a package: the tree follows at the next period.
        const locked = packages();
        Object.assign(locked, Object.fromEntries([node('left-pad', '1.3.0')]));
        await writeProject(storage, {wanted: {...dependencies, 'left-pad': '^1.3.0'}, lockRoots: {...roots(), 'left-pad': {requested: '^1.3.0', id: 'left-pad@1.3.0'}}, locked});
        timers.fire();
        await panel.settled();
        expect(reads()).toBeGreaterThan(before);
        expect(lines(panel.tree)).toContain('1|left-pad@1.3.0|^1.3.0|');
        expect(panel.summary.textContent).toBe('4 roots, 6 packages');
        expect(item(panel.tree, 'react-dom@19.3.1').getAttribute('aria-expanded')).toBe('false');
        expect(panel.selected).toBe('scheduler@0.20.0');
        expect(panel.detail.querySelector('h2').textContent).toBe('scheduler@0.20.0');
        expect(item(panel.tree, 'scheduler@0.20.0').tabIndex).toBe(0);

        // The selected package is under a folded one when the lock changes again: the folded package, which is shown, takes
        // the Tab key, and stays folded.
        key(item(panel.tree, '@app/kit@1.0.0'), 'ArrowLeft');
        Object.assign(locked, Object.fromEntries([node('is-odd', '3.0.1')]));
        await writeProject(storage, {wanted: {...dependencies, 'left-pad': '^1.3.0'}, lockRoots: {...roots(), 'left-pad': {requested: '^1.3.0', id: 'left-pad@1.3.0'}}, locked});
        await panel.refresh();
        expect(panel.summary.textContent).toBe('4 roots, 7 packages');
        expect(item(panel.tree, '@app/kit@1.0.0').getAttribute('aria-expanded')).toBe('false');
        expect([...panel.tree.querySelectorAll('[tabindex="0"]')]).toEqual([item(panel.tree, '@app/kit@1.0.0')]);
        expect(panel.selected).toBe('scheduler@0.20.0');

        // The package shown is removed: its detail goes with it.
        delete locked['scheduler@0.20.0'];
        locked['@app+kit@1.0.0'].dependencies = {'react-dom': 'react-dom@19.3.1'};
        await writeProject(storage, {wanted: {...dependencies, 'left-pad': '^1.3.0'}, lockRoots: {...roots(), 'left-pad': {requested: '^1.3.0', id: 'left-pad@1.3.0'}}, locked});
        await panel.refresh();
        expect(panel.selected).toBeNull();
        expect(panel.detail.hidden).toBeTrue();
        expect(lines(panel.tree)).not.toContain('2|scheduler@0.20.0|~0.20.0|2 versions');
        expect(lines(panel.tree)).toContain('3|scheduler@0.28.0|^0.28.0|');

        // The page that hides ends the reads.
        listeners.get('pagehide')();
        expect(timers.pending).toBeFalse();
        const asked = bridge.calls.length;
        await panel.refresh();
        expect(bridge.calls.length).toBe(asked);
    });

    it('shows the texts of a package as text, never as HTML, with their control characters made visible', async () => {
        const hostile = '<img src=x onerror="window.pwned=1"><script>window.pwned=1</script>';
        const locked = Object.fromEntries([node('evil', '1.0.0', {}, {description: hostile, license: `<b>MIT</b>\x1b[2J`,
            webfront: {bin: {'<i>run</i>': 'run.mjs', other: 'other.mjs'}}})]);
        // A path of the lock cannot hold < or >; an entity would still be decoded by an HTML parser.
        locked['evil@1.0.0'].files = ['package.json', '&lt;img src=x&gt;.js'];
        const {panel, body} = await open({wanted: {evil: '^1.0.0'}, lockRoots: {evil: {requested: '<u>^1</u>', id: 'evil@1.0.0'}}, locked,
            digest: {evil: '^1.0.0'}});
        panel.select('evil@1.0.0');
        expect(body.querySelector('img, script, b, i, u')).toBeNull();
        expect(window.pwned).toBeUndefined();
        expect(panel.detail.querySelector('p.description').textContent).toBe(hostile);
        expect(rowsOf(panel.detail).License).toBe('<b>MIT</b>^[[2J');
        expect(rowsOf(panel.detail).Executables).toBe('wf exec evil:<i>run</i>, wf exec evil:other');
        expect(panel.tree.querySelector('.package-range').textContent).toBe('<u>^1</u>');
        expect([...panel.detail.querySelectorAll('ul.files li')].at(-1).textContent).toBe('&lt;img src=x&gt;.js');
    });

    it('says when the project has no manifest, no package, or a lock that no longer matches the manifest', async () => {
        const none = await open({write: false});
        expect(none.status.textContent).toBe('This project has no webfront.project.json: wf init creates it.');
        expect(none.panel.tree.children.length).toBe(0);
        expect(none.panel.summary.textContent).toBe('');
        const empty = await open({wanted: {}, lockRoots: {}, locked: {}});
        expect(empty.status.textContent).toBe('No installed packages: wf install <package> adds one.');
        const changed = await open({digest: {react: '^18.0.0'}});
        expect(changed.status.textContent).toContain('Warning: The lock does not match the dependencies of webfront.project.json');
        expect(changed.panel.summary.textContent).toBe('3 roots, 5 packages');
    });

    it('says why the packages cannot be read, and shows them once they can', async () => {
        const storage = new Map();
        storage.set('webfront.project.json', encode(JSON.stringify({format: 1, name: 'app', version: '0.1.0'})));
        storage.set('webfront-packages.lock.json', encode('{"format":7}'));
        const {panel, status, timers} = await open({storage, write: false});
        expect(status.textContent).toBe('The packages cannot be read: Unsupported package lock');
        expect(status.classList.contains('error')).toBeTrue();
        expect(timers.delays).withContext('it tries again at the next period').toEqual([lockPeriod]);
        await writeProject(storage);
        timers.fire();
        await panel.settled();
        expect(status.classList.contains('error')).toBeFalse();
        expect(panel.summary.textContent).toBe('3 roots, 5 packages');
    });

    it('reads the manifest and the lock in the directory that the application chose', async () => {
        const storage = new Map();
        await writeProject(storage, {directory: 'js/app'});
        const config = () => JSON.stringify({environment: 'x', manifestDirectory: 'js/app'});
        const bridge = countingBridge(storage, {'webfront.developer.config': config});
        expect(await manifestDirectory(bridge)).toBe('js/app');
        expect(Object.keys((await readPackages(bridge)).lock.packages).length).toBe(5);
        // A bridge without the configuration (an older C++) reads the project root.
        expect(await manifestDirectory(countingBridge(new Map()))).toBeUndefined();
        const {panel} = await open({storage, write: false, extra: {'webfront.developer.config': config}});
        expect(panel.summary.textContent).toBe('3 roots, 5 packages');
    });

    it('is the panel of inspector.html?panel=typescript.packages, with the bridge of its host in a frame', async () => {
        const frame = document.createElement('iframe');
        document.body.append(frame);
        const storage = new Map();
        await writeProject(storage);
        const timers = manualTimers();
        try {
            const bridge = Object.assign(countingBridge(storage), {ready: Promise.resolve()});
            const view = {setTimeout: timers.setTimeout, clearTimeout: timers.clearTimeout, addEventListener() {}, document: frame.contentDocument,
                parent: {webfrontInspectorHost: {bridge}},
                location: {href: 'http://localhost/webfront/developer/environment/inspector.html?panel=typescript.packages&frame=1'}};
            const made = await main(view);
            expect(frame.contentDocument.title).toBe('WebFront inspector: TypeScript state: Packages');
            expect(frame.contentDocument.querySelectorAll('ul.packages [role=treeitem]').length).toBe(7);
            expect(frame.contentDocument.querySelector('input[type=search]')).not.toBeNull();
            made.stop();
        } finally { frame.remove(); }
    });
});
