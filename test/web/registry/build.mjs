// Builds the local npm registry of the tests with wf:
//
//   wf run test-registry
//
// The real packages are the verified archives of registry-archives/ (downloaded at configure time from
// test/web/registry-packages.json), kept byte for byte; the synthetic ones are generated. The registry's files go to
// build-registry/, next to an integrities.json of every package.
import {realPackage, syntheticPackages, registryFiles} from './fixture.mjs';

export default async function (context) {
    if (context.args.length) throw new Error('Usage: wf run test-registry');
    const {project} = context;
    const archives = (await project.list('registry-archives')).filter(name => name.endsWith('.tgz'));
    if (!archives.length) throw new Error('registry-archives holds no archive');
    const packages = [];
    for (const name of archives)
        packages.push(realPackage(await project.read(`registry-archives/${name}`), await project.read(`registry-archives/${name}.package.json`)));
    const {files, integrities} = await registryFiles([...packages, ...syntheticPackages()]);
    for (const [name, bytes] of files) await project.write(`build-registry/${name}`, bytes);
    await project.write('build-registry/integrities.json', JSON.stringify(integrities, null, 2) + '\n');
    context.console.log(`Registry: ${files.size} files, ${Object.keys(integrities).length} packages`);
}
