// Stages a built distribution into the static GitHub Pages site, keeping every published version, with wf:
//
//   wf run js/developer/scripts/site.mjs <distribution directory> <site directory>
//
// A published version is immutable: staging may only repeat identical bytes. Nothing is modified before every byte of
// an existing version has been compared.
import {versionName, versionPath, distributionFiles, sriOf} from '../tools/distribution.mjs';

const same = (a, b) => a.length === b.length && a.every((byte, index) => byte === b[index]);

async function readDistribution(project, distribution) {
    const manifest = JSON.parse(await project.readText(`${distribution}/provision.json`));
    const input = new Map();
    for (const name of distributionFiles(manifest)) input.set(name, await project.read(`${distribution}/${name}`));
    const archives = [[input.get('environment.tgz'), manifest.integrity],
        ...Object.values(manifest.starters).map(starter => [input.get(starter.archive), starter.integrity])];
    for (const [bytes, integrity] of archives)
        if (await sriOf(bytes) !== integrity) throw new Error('Distribution archive checksum mismatch');
    return {manifest, input};
}

const entries = async (project, directory) => (await project.list(directory)).map(name => name.replace(/\/$/, ''));

// A version is published when its directory is listed under the site's developer directory. A missing directory cannot be
// listed, so each level is looked up in its parent (the site itself, in the project root or in its parent directory).
async function published(project, site, version) {
    const slash = site.lastIndexOf('/');
    const [parent, name] = slash < 0 ? ['', site] : [site.slice(0, slash), site.slice(slash + 1)];
    if (!(await entries(project, parent)).includes(name)) return false;
    if (!(await entries(project, site)).includes('developer')) return false;
    return (await entries(project, `${site}/developer`)).includes(version);
}

// A published version is immutable: reruns may only repeat identical bytes.
async function publishVersion(project, input, site, version) {
    const destination = `${site}/developer/${version}`;
    if (!await published(project, site, version)) {
        for (const [name, bytes] of input) await project.write(`${destination}/${name}`, bytes);
        return;
    }
    // Check every byte before modifying anything.
    for (const [name, bytes] of input) {
        const existing = await project.read(`${destination}/${name}`).catch(() => null);
        if (!existing || !same(bytes, existing))
            throw new Error(`Published version ${version} differs; increment the distribution version`);
    }
}

async function versionRow(project, site, version) {
    const manifest = JSON.parse(await project.readText(`${site}/${versionPath(version)}provision.json`));
    const links = distributionFiles(manifest).map(name => `<a href="developer/${version}/${name}">${name}</a>`).join(' · ');
    return `<li>${version}: ${links}</li>`;
}

async function writeIndex(project, site) {
    const versions = (await entries(project, `${site}/developer`)).filter(name => versionName.test(name)).sort();
    await project.write(`${site}/.nojekyll`, '');
    const rows = [];
    for (const version of versions) rows.push(await versionRow(project, site, version));
    await project.write(`${site}/index.html`, `<!doctype html>\n<html lang="en"><meta charset="utf-8"><title>WebFront developer packages</title><h1>WebFront developer packages</h1><p>Use a version-pinned bootstrap header and SHA-512. React and its dependencies are downloaded separately from npm.</p><ul>${rows.join('')}</ul></html>\n`);
}

export default async function (context) {
    const [distribution, site] = context.args;
    if (!distribution || !site || context.args.length !== 2) throw new Error('Usage: wf run site <distribution directory> <site directory>');
    const {project} = context;
    const {manifest, input} = await readDistribution(project, distribution);
    const relative = versionPath(manifest.version);
    await publishVersion(project, input, site, manifest.version);
    await writeIndex(project, site);
    context.console.log(`Staged developer packages ${manifest.version} in ${site}/${relative}`);
}
