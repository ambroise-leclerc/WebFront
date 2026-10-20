// Dedicated prereleases: retries verify existing assets, never replace an existing release.
import {execFileSync} from 'node:child_process';
import {readFile, mkdtemp, rm} from 'node:fs/promises';
import {join, resolve} from 'node:path';
import {tmpdir} from 'node:os';
const [tag, distribution] = process.argv.slice(2);
const metadata = JSON.parse(await readFile(new URL('./distribution.json', import.meta.url)));
const repository = process.env.GITHUB_REPOSITORY;
if (tag !== `developer-v${metadata.version}` || !/^[\w.-]+\/[\w.-]+$/.test(repository ?? ''))
    throw new Error('Expected the dedicated distribution tag and GITHUB_REPOSITORY');
const gh = (...args) => execFileSync('gh', args, {encoding: 'utf8'});
const files = ['environment.tgz', 'pi-starter.tgz', 'provision.json', 'DeveloperReleaseData.hpp'];
let existing = false;
try {
    gh('api', `repos/${repository}/releases/tags/${tag}`);
    existing = true;
} catch (error) {
    if (!String(error.stderr).includes('(HTTP 404)')) throw error;
}
if (existing) {
    const directory = await mkdtemp(join(tmpdir(), 'webfront-release-'));
    try {
        gh('release', 'download', tag, '--repo', repository, '--dir', directory,
            ...files.flatMap(name => ['--pattern', name]));
        for (const name of files)
            if (!(await readFile(join(directory, name))).equals(await readFile(join(distribution, name))))
                throw new Error(`Existing release asset ${name} differs; choose a new version`);
        console.log('Existing release assets match the distribution');
    } finally { await rm(directory, {recursive: true, force: true}); }
} else {
    console.log(gh('release', 'create', tag, '--repo', repository, '--verify-tag', '--prerelease',
        '--title', `WebFront developer packages ${metadata.version}`,
        '--notes-file', resolve('tools/developer/RELEASE_NOTES.md'), ...files.map(name => resolve(distribution, name))));
}
