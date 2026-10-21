// A token-driven branch push does not trigger Pages automatically: request the build explicitly.
// With --check, only verify the configured Pages source (before anything is pushed to gh-pages).
import {execFileSync} from 'node:child_process';
const repository = process.env.GITHUB_REPOSITORY;
if (!/^[\w.-]+\/[\w.-]+$/.test(repository ?? '')) throw new Error('GITHUB_REPOSITORY is required');
const api = (...args) => execFileSync('gh', ['api', ...args], {encoding: 'utf8'});
const configuration = JSON.parse(api(`repos/${repository}/pages`));
if (configuration.build_type !== 'legacy' || configuration.source?.branch !== 'gh-pages' || configuration.source?.path !== '/')
    throw new Error('Configure GitHub Pages to build from gh-pages / before publishing; an existing site is never reconfigured automatically');
if (!process.argv.includes('--check')) console.log(api('--method', 'POST', `repos/${repository}/pages/builds`));
