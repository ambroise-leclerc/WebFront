// Keep the checked-in header format and the pinned compressor independent of the host.
import { renderAsset } from '../tools/asset-header.mjs';
import { mkdirSync, readFileSync, realpathSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';

export { renderAsset };
export const STALE_ASSET_MESSAGE = 'is stale; run the webfront-embedded-assets target';

function main() {
    const { values } = parseArgs({ options: {
        input: { type: 'string' }, struct: { type: 'string' },
        output: { type: 'string' }, check: { type: 'string' }, gzip: { type: 'boolean' },
    } });
    if (!values.input || !values.struct || (!values.output && !values.check))
        throw new Error('Supply --input, --struct and --output or --check');
    const contents = renderAsset(readFileSync(values.input), values.struct, values.gzip);
    if (values.check) {
        if (!readFileSync(values.check).equals(Buffer.from(contents)))
            throw new Error(`${values.check} ${STALE_ASSET_MESSAGE}`);
    } else {
        mkdirSync(dirname(values.output), { recursive: true });
        writeFileSync(values.output, contents);
    }
}

if (process.argv[1] && import.meta.url === pathToFileURL(realpathSync(resolve(process.argv[1]))).href) {
    try {
        main();
    } catch (error) {
        console.error(error.message);
        process.exitCode = 1;
    }
}
