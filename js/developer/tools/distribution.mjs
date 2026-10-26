// The pure rules of the developer distribution (versions, URLs, the pinned header, the published files). They use
// no host API: the wf scripts of js/developer/scripts and the Node maintenance tools share them.

function isHttpsDirectory(url) {
    if (url.protocol !== 'https:') return false;
    if (url.username || url.password) return false;
    return !url.search && !url.hash;
}

export function baseUrl(value) {
    const url = new URL(value);
    if (!isHttpsDirectory(url)) throw new Error('Distribution base URL must be an HTTPS directory URL');
    if (!url.pathname.endsWith('/')) url.pathname += '/';
    return url.href;
}

export const versionName = /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/;

export function versionPath(version) {
    if (!versionName.test(version)) throw new Error('Invalid distribution version');
    return `developer/${version}/`;
}

export function bootstrapHeader(version, base, integrity) {
    if (!/^sha512-[A-Za-z0-9+/]{86}==$/.test(integrity)) throw new Error('Invalid distribution SHA-512');
    const url = new URL(versionPath(version) + 'environment.tgz', baseUrl(base));
    return `#pragma once

// Pinned developer distribution. Regenerate with wf run build <directory> --refresh-bootstrap.
#include <string_view>

namespace webfront::developer::release {
inline constexpr std::string_view version{${JSON.stringify(version)}};
inline constexpr std::string_view environment{${JSON.stringify(url.href)}};
inline constexpr std::string_view integrity{${JSON.stringify(integrity)}};
} // namespace webfront::developer::release
`;
}

const validArchive = name => typeof name === 'string' && /^[a-z0-9-]+-starter\.tgz$/.test(name);

// A manifest may add starters without changing the publication machinery.
export function distributionFiles(manifest) {
    const archives = Object.values(manifest.starters ?? {}).map(starter => starter.archive);
    if (archives.length === 0 || !archives.every(validArchive)) throw new Error('Invalid distribution starter archives');
    if (new Set(archives).size !== archives.length) throw new Error('Invalid distribution starter archives');
    return ['environment.tgz', ...archives, 'provision.json', 'DeveloperReleaseData.hpp'];
}

/** The SRI value of the SHA-512 of some bytes, with the Web Crypto API (a Worker, a page or Node). */
export async function sriOf(bytes) {
    return 'sha512-' + btoa(String.fromCharCode(...new Uint8Array(await crypto.subtle.digest('SHA-512', bytes))));
}
