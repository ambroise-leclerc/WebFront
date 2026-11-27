// The Packages panel of the inspectors (#683, ADR-027): the locked packages as a tree that can be walked, what wf list --tree,
// wf why and wf info say as text. It reads the manifest and the lock through the bridge (the reads of the project, which no
// function of the inspectors replaces), never the network, and writes nothing; it reads them again when either file changes
// (wf install, wf restore). Everything shown comes from the package.json of packages, which are not trusted: it is set as
// text, never as HTML, with its control characters made visible (dependencies.mjs).
import {call} from './io.mjs';
import {setManifestDirectory, manifestPath, lockPath} from './manifest.mjs';
import {loadProject, desynchronized} from './project.mjs';
import {dependencyGraph, packageDetails, whyChains, chainLimit} from './dependencies.mjs';
import {inline} from './terminal.mjs';

/** How often the panel asks whether the manifest or the lock changed: one call, the files are read only after a change. */
export const lockPeriod = 1000;

/**
 * The directory of the manifest and of the lock that the application chose, as the terminal reads it from the bootstrap's
 * configuration; the project root when the application says nothing, or when the configuration cannot be read.
 */
export async function manifestDirectory(bridge) {
    try { return JSON.parse(await call(bridge, 'config'))?.manifestDirectory; }
    catch { return undefined; }
}

/** The project of the page: its manifest, its lock and the token of their state (project.mjs), read where the application said. */
export async function readPackages(bridge) {
    setManifestDirectory(await manifestDirectory(bridge));
    return loadProject(bridge);
}

/** The token of the state that readPackages() read (`project.token`): it changes when the manifest or the lock does. */
export const packagesState = bridge => call(bridge, 'state', `${manifestPath}\n${lockPath}`);

/**
 * The locked packages whose name contains `query`, whatever the case, sorted by name then version; all of them for no query.
 * Each is {id, label, root, versions}: whether the manifest asks for it, and how many versions of its name are locked.
 */
export function searchPackages(lock, query) {
    const wanted = String(query ?? '').trim().toLowerCase();
    const versions = new Map();
    const all = Object.entries(lock?.packages ?? {});
    for (const [, node] of all) versions.set(node.name, (versions.get(node.name) ?? 0) + 1);
    const roots = new Set(Object.values(lock?.roots ?? {}).map(root => root.id));
    const order = (a, b) => a < b ? -1 : a > b ? 1 : 0;
    return all.filter(([, node]) => node.name.toLowerCase().includes(wanted))
        .sort(([, a], [, b]) => order(a.name, b.name) || order(a.version, b.version))
        .map(([id, node]) => ({id, label: inline(`${node.name}@${node.version}`), root: roots.has(id), versions: versions.get(node.name)}));
}

/** The path of a node of the tree, which names one of the places a package is shown at: the ids from its root down. */
const pathOf = (parent, id) => parent ? `${parent}>${id}` : id;

export const packagesStyles = `
.packages-layout{display:grid;grid-template-columns:minmax(0,1fr);gap:12px}
@media (min-width:720px){.packages-layout{grid-template-columns:minmax(0,1fr) minmax(0,1fr)}}
ul.packages,ul.packages ul,ul.matches,ol.chains,ul.files{list-style:none;margin:0;padding:0}
ul.packages ul{padding-left:18px}
ul.packages li>.row{display:flex;flex-wrap:wrap;align-items:center;gap:2px 8px;min-height:28px;padding:0 4px;border-radius:4px;cursor:pointer}
ul.packages li:focus{outline:none}
ul.packages li:focus-visible>.row{outline:2px solid #d7fa85;outline-offset:-2px}
ul.packages li[aria-selected=true]>.row,ul.matches button[aria-pressed=true]{background:#2f3a50}
ul.packages .twisty{flex:0 0 14px;color:#a9b3c4;text-align:center}
ul.packages li[aria-expanded=false]>ul{display:none}
.package-name{font-family:ui-monospace,monospace;font-size:13px;overflow-wrap:anywhere}
.package-range{color:#a9b3c4;font-family:ui-monospace,monospace;font-size:12px}
.badge{padding:0 6px;border:1px solid #46536a;border-radius:8px;color:#a9b3c4;font-size:11px;white-space:nowrap}
.badge.peer{color:#8fd0ff;border-color:#8fd0ff}
.badge.versions{color:#ffbd2e;border-color:#ffbd2e}
ul.matches button,.package-detail button.package-link{min-height:28px;padding:0 6px;border:0;border-radius:4px;background:none;color:#8fd0ff;font:13px ui-monospace,monospace;text-align:left;cursor:pointer;overflow-wrap:anywhere}
ul.matches li{display:flex;flex-wrap:wrap;align-items:center;gap:2px 8px}
ul.matches button:focus-visible,.package-detail button:focus-visible{outline:2px solid #d7fa85;outline-offset:-2px}
.package-detail{min-width:0;padding:8px 12px;border:1px solid #2f3a50;border-radius:4px;align-self:start}
.package-detail h2:focus{outline:none}
.package-detail h2{margin:0 0 4px;font:600 14px ui-monospace,monospace;overflow-wrap:anywhere}
.package-detail h3{margin:12px 0 4px;font-size:12px;font-weight:600;color:#a9b3c4}
.package-detail p{margin:0 0 8px}
.package-detail dl{display:grid;grid-template-columns:auto minmax(0,1fr);gap:4px 12px;margin:0}
.package-detail dt{color:#a9b3c4;font-size:12px}
.package-detail dd{margin:0;overflow-wrap:anywhere}
.package-detail .code{font-family:ui-monospace,monospace;font-size:12px}
.package-detail .note,ul.matches .note{color:#a9b3c4;font-size:12px}
ol.chains li{padding:2px 0;border-bottom:1px solid #2f3a50}
ul.files{max-height:12em;overflow:auto;font-family:ui-monospace,monospace;font-size:12px}`;

/**
 * Shows the panel in `body`. `load(bridge)` reads the project and `state(bridge)` the token of its state (tests replace
 * them). Resolves, once the packages are shown or the first read failed, with the parts of the panel and:
 *   `select(id)`: shows a package in the detail and at its first place in the tree; false when it is not locked;
 *   `refresh()`: asks whether the lock changed and reads it again if it did (what the timer does every `lockPeriod`);
 *   `settled()`: resolves once the read in progress is over; `stop()`: ends the reads (the page that hides calls it).
 */
export async function showPackagesPanel({page, view, body, status, bridge, load = readPackages, state = packagesState}) {
    const element = (tag, className, text) => {
        const made = page.createElement(tag);
        if (className) made.className = className;
        if (text !== undefined) made.textContent = text;  // text of a package: never parsed as HTML
        return made;
    };
    const search = element('input');
    search.type = 'search';
    search.placeholder = 'Search a package by name';
    search.setAttribute('aria-label', 'Search a package by name');
    const toolbar = element('div', 'toolbar');
    toolbar.append(search);
    const summary = element('p', 'summary');
    const layout = element('div', 'packages-layout');
    const lists = element('div');
    const tree = element('ul', 'packages');
    tree.setAttribute('role', 'tree');
    tree.setAttribute('aria-label', 'Locked packages, under the roots of the manifest');
    const matches = element('ul', 'matches');
    matches.setAttribute('aria-label', 'Packages found');
    matches.hidden = true;
    lists.append(tree, matches);
    const detail = element('section', 'package-detail');
    detail.setAttribute('aria-label', 'Package');
    detail.hidden = true;
    layout.append(lists, detail);
    body.append(toolbar, summary, layout);

    let project = null, graph = {roots: [], packages: 0}, selected = null;
    const collapsed = new Set();   // by path, so that a package folded at one place stays folded when the lock is read again
    const lock = () => project?.lock ?? null;
    const items = () => [...tree.querySelectorAll('[role=treeitem]')];
    const parentItem = item => item.parentElement.closest('[role=treeitem]');
    const visible = () => items().filter(item => {
        for (let above = parentItem(item); above; above = parentItem(above))
            if (above.getAttribute('aria-expanded') === 'false') return false;
        return true;
    });
    // The place where a package is opened, its dependencies listed under it; any place when it has none.
    const first = id => items().find(item => item.dataset.package === id && item.dataset.deduped !== 'true') ?? null;
    const focusable = item => {
        for (const other of items()) other.tabIndex = other === item ? 0 : -1;
    };
    const setExpanded = (item, expanded) => {
        if (!item.hasAttribute('aria-expanded')) return;
        item.setAttribute('aria-expanded', String(expanded));
        item.querySelector('.twisty').textContent = expanded ? '▾' : '▸';
        if (expanded) collapsed.delete(item.dataset.path); else collapsed.add(item.dataset.path);
    };
    const reveal = item => {
        for (let above = parentItem(item); above; above = parentItem(above)) setExpanded(above, true);
    };

    const badge = (className, text, title) => {
        const made = element('span', `badge ${className}`, text);
        made.title = title;
        return made;
    };
    const versionsBadge = count => badge('versions', `${count} versions`, 'Several versions of this package are locked');
    const link = (id, label) => {
        const button = element('button', 'package-link', label);
        button.type = 'button';
        button.dataset.package = id;
        button.addEventListener('click', () => {
            // A link of the detail is replaced with it: the focus goes to the title of the package it leads to, not to the page.
            const replaced = detail.contains(button);
            select(id);
            if (replaced) detail.querySelector('h2')?.focus();
        });
        return button;
    };

    function renderDetail() {
        const current = lock();
        if (!selected || !current?.packages[selected]) {
            selected = null;
            detail.hidden = true;
            detail.replaceChildren();
            return;
        }
        const details = packageDetails(current, selected);
        const row = (term, ...values) => {
            const value = element('dd');
            value.append(...values);
            return [element('dt', '', term), value];
        };
        const links = (list, note) => list.length ? list.flatMap((entry, index) => [
            ...(index ? [', '] : []), link(entry.id, entry.label), ...(note(entry) ? [element('span', 'note', ` (${note(entry)})`)] : [])]) : ['none'];
        const rows = element('dl');
        rows.append(
            ...row('Root', details.roots.length ? `yes, requested ${details.roots.join(', ')}` : 'no, installed for another package'),
            ...row('License', details.license),
            ...row('Integrity', element('span', 'code', details.integrity)),
            ...row('Directory', element('span', 'code', details.directory)),
            ...row('Dependencies', ...links(details.dependencies, entry => entry.request)),
            ...row('Dependents', ...links(details.dependents, () => '')),
            ...(details.executables.length ? row('Executables', ...details.executables.flatMap(({command}, index) =>
                [...(index ? [', '] : []), element('span', 'code', command)])) : []));
        const {chains, limited} = whyChains(current, selected);
        const why = element('ol', 'chains');
        why.setAttribute('aria-label', 'Why it is installed');
        for (const chain of chains) {
            const item = element('li');
            chain.forEach((step, index) => item.append(...(index ? [' > '] : []), link(step.id, step.label),
                ...(step.note ? [element('span', 'note', ` (${step.note})`)] : [])));
            why.append(item);
        }
        const files = element('details', 'package-files');
        files.append(element('summary', '', `${details.files.length} ${details.files.length === 1 ? 'file' : 'files'}`));
        const fileList = element('ul', 'files');
        for (const path of details.files) fileList.append(element('li', '', path));
        files.append(fileList);
        const title = element('h2', '', details.label);
        title.tabIndex = -1;
        detail.replaceChildren(title, ...(details.description ? [element('p', 'description', details.description)] : []), rows,
            element('h3', '', 'Why it is installed (wf why)'), why,
            ...(chains.length ? [] : [element('p', 'note', 'Nothing requires it: wf install --update resolves the manifest again.')]),
            ...(limited ? [element('p', 'note', `Only the first ${chainLimit} chains are shown.`)] : []),
            element('h3', '', 'Files'), files);
        detail.hidden = false;
    }

    function markSelected() {
        for (const item of items()) item.setAttribute('aria-selected', String(item.dataset.package === selected));
        for (const button of matches.querySelectorAll('button')) button.setAttribute('aria-pressed', String(button.dataset.package === selected));
    }

    /** Shows a locked package: its detail, and the place of the tree where its dependencies are listed. */
    function select(id) {
        if (!lock()?.packages[id]) return false;
        selected = id;
        const item = first(id);
        if (item) {
            reveal(item);
            focusable(item);
            if (!tree.hidden) item.scrollIntoView?.({block: 'nearest'});
        }
        markSelected();
        renderDetail();
        return true;
    }

    function renderTree() {
        const build = (entry, parent, level) => {
            const item = element('li');
            const path = pathOf(parent, entry.id);
            item.setAttribute('role', 'treeitem');
            item.setAttribute('aria-level', String(level));
            item.setAttribute('aria-selected', 'false');
            item.tabIndex = -1;
            item.dataset.package = entry.id;
            item.dataset.path = path;
            if (entry.deduped) item.dataset.deduped = 'true';
            const twisty = element('span', 'twisty', entry.children.length ? (collapsed.has(path) ? '▸' : '▾') : '');
            twisty.setAttribute('aria-hidden', 'true');
            const row = element('div', 'row');
            row.append(twisty, element('span', 'package-name', entry.label));
            if (entry.range) row.append(element('span', 'package-range', entry.range));
            if (entry.kind === 'peer') row.append(badge('peer', 'peer', 'A peer dependency: the package uses the one its dependent installs'));
            if (entry.kind === 'optional') row.append(badge('optional', 'optional', 'An optional dependency'));
            if (entry.deduped) row.append(badge('deduped', 'deduped', 'Its dependencies are listed where it comes first: the Right arrow goes there'));
            if (entry.versions > 1) row.append(versionsBadge(entry.versions));
            item.append(row);
            if (entry.children.length) {
                item.setAttribute('aria-expanded', String(!collapsed.has(path)));
                const group = element('ul');
                group.setAttribute('role', 'group');
                for (const child of entry.children) group.append(build(child, path, level + 1));
                item.append(group);
            }
            return item;
        };
        tree.replaceChildren(...graph.roots.map(root => build(root, '', 1)));
        // Paths that no longer exist are forgotten, so that the set stays bounded by the tree shown.
        const paths = new Set(items().map(item => item.dataset.path));
        for (const path of [...collapsed]) if (!paths.has(path)) collapsed.delete(path);
        // The item that takes the Tab key is one that is shown: the selected package, or the folded package that hides it.
        let start = (selected && first(selected)) || items()[0];
        for (let above = start && parentItem(start); above; above = parentItem(above))
            if (above.getAttribute('aria-expanded') === 'false') start = above;
        if (start) focusable(start);
    }

    function renderMatches() {
        const query = search.value.trim();
        tree.hidden = Boolean(query);
        matches.hidden = !query;
        if (!query) {
            matches.replaceChildren();
            return;
        }
        const found = searchPackages(lock(), query);
        matches.replaceChildren(...found.map(match => {
            const item = element('li');
            const button = link(match.id, match.label);
            button.setAttribute('aria-pressed', String(match.id === selected));
            item.append(button);
            if (match.root) item.append(badge('root', 'root', 'A root of the manifest'));
            if (match.versions > 1) item.append(versionsBadge(match.versions));
            return item;
        }));
        if (!found.length) matches.append(element('li', 'note', 'No locked package has this name.'));
    }

    function describe() {
        status.classList.remove('error');
        const count = graph.packages, roots = graph.roots.length;
        summary.textContent = roots ? `${roots} ${roots === 1 ? 'root' : 'roots'}, ${count} ${count === 1 ? 'package' : 'packages'}` : '';
        if (project.status === 'no-manifest' && !roots) status.textContent = 'This project has no webfront.project.json: wf init creates it.';
        else if (project.status === 'desynchronized') status.textContent = `Warning: ${desynchronized}.`;
        else status.textContent = roots ? '' : 'No installed packages: wf install <package> adds one.';
    }

    function render() {
        graph = dependencyGraph(lock());
        renderTree();
        renderMatches();
        markSelected();
        renderDetail();
        describe();
    }

    tree.addEventListener('click', event => {
        const item = event.target.closest?.('[role=treeitem]');
        if (!item) return;
        focusable(item);
        if (event.target.classList?.contains('twisty') && item.hasAttribute('aria-expanded')) {
            setExpanded(item, item.getAttribute('aria-expanded') === 'false');
            return;
        }
        selected = item.dataset.package;  // the place that was clicked keeps the focus, wherever the package is opened
        markSelected();
        renderDetail();
        item.focus?.();
    });
    tree.addEventListener('keydown', event => {
        const item = event.target.closest?.('[role=treeitem]');
        if (!item) return;
        const shown = visible(), index = shown.indexOf(item);
        const go = target => {
            if (!target) return;
            focusable(target);
            target.focus();
        };
        const expanded = item.getAttribute('aria-expanded');
        if (event.key === 'ArrowDown') go(shown[index + 1]);
        else if (event.key === 'ArrowUp') go(shown[index - 1]);
        else if (event.key === 'Home') go(shown[0]);
        else if (event.key === 'End') go(shown.at(-1));
        else if (event.key === 'ArrowRight') {
            if (expanded === 'false') setExpanded(item, true);
            else if (expanded === 'true') go(shown[index + 1]);
            else if (item.dataset.deduped === 'true') {
                const place = first(item.dataset.package);
                if (place) reveal(place);
                go(place);
            }
        } else if (event.key === 'ArrowLeft') {
            if (expanded === 'true') setExpanded(item, false);
            else go(parentItem(item));
        } else if (event.key === 'Enter' || event.key === ' ') {
            selected = item.dataset.package;
            markSelected();
            renderDetail();
        } else return;
        event.preventDefault();
    });
    search.addEventListener('input', renderMatches);

    // One chain of reads: the token of the state is asked every period, and the files are read only when it changed.
    let timer = null, stopped = false, running = null;
    const fail = error => {
        if (stopped) return;
        status.classList.add('error');
        status.textContent = `The packages cannot be read: ${error?.message ?? error}`;
    };
    async function read() {
        const loaded = await load(bridge);
        if (stopped) return;
        project = loaded;
        render();
    }
    async function check() {
        // A project that could not be read is read again; one that was is read again when its manifest or its lock changed.
        if (project && await state(bridge) === project.token) {
            if (!stopped && status.classList.contains('error')) describe();
            return;
        }
        if (!stopped) await read();
    }
    function schedule() {
        if (stopped || timer !== null || running !== null) return;
        timer = view.setTimeout(() => {
            timer = null;
            refresh();
        }, lockPeriod);
    }
    const follow = reading => running = reading.catch(fail).finally(() => {
        running = null;
        schedule();
    });
    function refresh() {
        if (stopped) return Promise.resolve();
        if (running) return running;
        view.clearTimeout(timer);
        timer = null;
        return follow(check());
    }
    const stop = () => {
        stopped = true;
        view.clearTimeout(timer);
        timer = null;
    };
    view.addEventListener('pagehide', stop);
    await follow(read());
    return {search, tree, matches, detail, summary, select, refresh, stop, settled: () => running ?? Promise.resolve(),
        get selected() { return selected; }};
}
