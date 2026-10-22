import {readFile} from 'node:fs/promises';
const distribution = JSON.parse(await readFile(new URL('./distribution.json', import.meta.url)));
if (process.argv[2] !== `developer-v${distribution.version}`)
    throw new Error(`Expected dedicated tag developer-v${distribution.version}`);
