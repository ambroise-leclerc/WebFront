// The deterministic npm archive of a package kept in js/<directory>: its package.json and the files it declares. Shared by
// wf run react-package, wf run services-package and the test registry, so that all of them write the same bytes.
import {tar, storedGzip} from '../tools/archive.mjs';
import {sriOf} from '../tools/distribution.mjs';

export async function packageArchive(project, directory, slug) {
    const root = `js/${directory}`;
    const manifest = JSON.parse(await project.readText(`${root}/package.json`));
    const files = new Map([['package/package.json', await project.read(`${root}/package.json`)]]);
    for (const file of manifest.files)
        files.set(`package/${file}`, await project.read(`${root}/${file}`));
    const bytes = storedGzip(tar(files));
    const archive = `webfront-${slug}-${manifest.version}.tgz`;
    return {manifest, bytes, archive, integrity: await sriOf(bytes)};
}

/** The body of a `wf run <slug>-package` script: writes build-<slug>-package/ and reports it. */
export async function writePackage({project, args, console}, directory, slug, label) {
    if (args.length) throw new Error(`Usage: wf run ${slug}-package`);
    const {bytes, archive, integrity} = await packageArchive(project, directory, slug);
    await project.write(`build-${slug}-package/${archive}`, bytes);
    await project.write(`build-${slug}-package/integrity.txt`, integrity + '\n');
    console.log(`${label}: build-${slug}-package/${archive}`);
}
