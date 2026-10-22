// wf check, page side: the locked typescript must be a supported version; the Worker reads the project and the
// package cache through a host that answers only reads and listings, under the stop mechanism of wf run.
import semver from 'semver';
import {call, readProject} from './io.mjs';
import {lockedPackage, defaultTimeout, timeoutBounds} from './scripts.mjs';
import {safePackagePath} from './paths.mjs';

/** The typescript versions wf check runs: those whose lib/typescript.js provides the compiler API. */
export const supportedTypeScript = '>=5.0.0 <7.0.0';
const decoder = new TextDecoder();

/** The locked typescript package wf check runs, as [id, node]; a missing or unsupported one is refused. */
export function typescriptPackage(lock) {
    const locked = lockedPackage(lock, 'typescript');
    if (!locked) throw new Error('wf check needs the typescript package: run wf install typescript@6');
    const [id, node] = locked;
    if (!semver.satisfies(node.version, supportedTypeScript)) {
        const native = semver.major(node.version) >= 7 ? ', whose compiler is a native program without the JavaScript API' : '';
        throw new Error(`wf check supports typescript 5.x and 6.x (${supportedTypeScript}), not ${id}${native}: run wf install typescript@6`);
    }
    if (!node.files.includes('lib/typescript.js')) throw new Error(`${id} has no lib/typescript.js`);
    return locked;
}

// wf check [--timeout <seconds>]
export function parseCheckOptions(args) {
    if (!args.length) return {timeout: defaultTimeout};
    const timeout = Number(args[1]);
    if (args[0] !== '--timeout' || args.length !== 2) throw new Error('Usage: wf check [--timeout <seconds>]');
    if (!Number.isInteger(timeout) || timeout < timeoutBounds[0] || timeout > timeoutBounds[1])
        throw new Error(`--timeout expects whole seconds from ${timeoutBounds[0]} to ${timeoutBounds[1]}`);
    return {timeout};
}

/** The page side of a check: project reads and listings only, refused once stopped. */
export function checkHost(bridge) {
    let stopped = false;
    const operations = {
        read: path => readProject(bridge, safePackagePath(path)),
        list: async (directory = '') => JSON.parse(decoder.decode(await call(bridge, 'list', directory && safePackagePath(directory)))),
    };
    return {
        publications: [],
        async handle({operation, args}) {
            if (stopped) throw new Error('The check was stopped');
            if (!Object.hasOwn(operations, operation)) throw new Error(`Unknown check operation: ${operation}`);
            return operations[operation](...args);
        },
        async stop() { stopped = true; },
    };
}
