// wf run react-package: prepare a publishable npm archive; this never publishes it.
import {tar, storedGzip} from '../tools/archive.mjs';
import {sriOf} from '../tools/distribution.mjs';

export async function reactPackage(project) {
    const manifest = JSON.parse(await project.readText('js/react/package.json'));
    const files = new Map([['package/package.json', await project.read('js/react/package.json')]]);
    for (const file of manifest.files)
        files.set(`package/${file}`, await project.read(`js/react/${file}`));
    const bytes = storedGzip(tar(files));
    const archive = `webfront-react-${manifest.version}.tgz`;
    return {manifest, bytes, archive, integrity: await sriOf(bytes)};
}

export default async function({project, args, console}) {
    if (args.length) throw new Error('Usage: wf run react-package');
    const {bytes, archive, integrity} = await reactPackage(project);
    await project.write(`build-react-package/${archive}`, bytes);
    await project.write('build-react-package/integrity.txt', integrity + '\n');
    console.log(`React adapter: build-react-package/${archive}`);
}
