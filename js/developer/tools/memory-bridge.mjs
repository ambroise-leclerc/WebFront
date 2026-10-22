// An in-memory project behind the webfront.developer.* callbacks, with the store's directory replacement, group
// publication and precondition. Shared by the Node tests; it uses only browser APIs, so that the tests can move to
// a browser runner.
export function memoryBridge(storage = new Map()) {
    const transactions = new Map();
    let next = 0;
    // The state of a file is its content: compared synchronously, so that check and publication stay one step.
    const stateOf = paths => JSON.stringify(paths.map(path => storage.has(path) ? [...storage.get(path)] : null));
    const inside = destination => [...storage.keys()].filter(key => key.startsWith(destination + '/'));
    const operations = {
        read: (path, offset) => {
            if (!storage.has(path)) throw new Error('Cannot read project file: ' + path);
            return storage.get(path).subarray(offset, offset + 256 * 1024);
        },
        // Like Store::list: the sorted names of a directory, one per line, directories ending with '/'.
        list: directory => {
            const prefix = directory ? directory + '/' : '';
            const names = new Set([...storage.keys()].filter(key => key.startsWith(prefix))
                .map(key => key.slice(prefix.length)).map(rest => rest.includes('/') ? rest.split('/')[0] + '/' : rest));
            if (!names.size) throw new Error('Cannot list project directory: ' + directory);
            return new TextEncoder().encode([...names].sort().join('\n'));
        },
        begin: (destination, directory) => { transactions.set(String(++next), {destination, directory, files: new Map(), replace: true}); return String(next); },
        beginNew: (destination, directory) => { transactions.set(String(++next), {destination, directory, files: new Map(), replace: false}); return String(next); },
        write: (id, path, offset, bytes) => {
            const files = transactions.get(id).files;
            const previous = files.get(path) ?? new Uint8Array();
            if (previous.length !== offset) throw new Error('Non-sequential developer write');
            files.set(path, new Uint8Array([...previous, ...bytes]));
        },
        commit: id => {
            const {destination, directory, files, replace} = transactions.get(id);
            if (!replace && (storage.has(destination) || inside(destination).length))
                throw new Error('Starter destination already exists');
            // A directory publication replaces the whole directory, as the store does.
            if (directory) inside(destination).forEach(key => storage.delete(key));
            for (const [path, bytes] of files) storage.set(directory ? `${destination}/${path}` : destination, bytes);
            transactions.delete(id);
        },
        abort: id => { transactions.delete(id); },
        state: paths => stateOf(paths.split('\n')),
        commitGroup: (ids, paths, expected) => {
            if (stateOf(paths.split('\n')) !== expected) throw new Error('The project changed since it was read; run the command again');
            for (const id of ids.split('\n')) {
                const {destination, files} = transactions.get(id);
                storage.set(destination, files.get('output'));
                transactions.delete(id);
            }
        },
    };
    const bridge = {cppFunction: name => async (...args) => operations[name.replace('webfront.developer.', '')](...args)};
    return {bridge, storage, pending: () => transactions.size};
}
