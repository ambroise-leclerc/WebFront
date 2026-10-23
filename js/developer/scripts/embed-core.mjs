// Regenerates the headers of the core's embedded assets from their readable sources, with wf:
//
//   wf run js/developer/scripts/embed-core.mjs
//
// The bytes are those of the CMake target webfront-embedded-assets: the same renderer, no compression.
import {renderAsset} from '../tools/asset-header.mjs';

// [readable source, generated struct]: the closed list of ADR-016, minus the pinned release data.
export const coreAssets = [
    ['js/core/WebFront.js', 'WebFrontJsData'],
    ['js/core/fallback/index.html', 'FallbackIndexHtmlData'],
    ['js/core/fallback/webfront-fallback.mjs', 'FallbackModuleData'],
    ['js/core/favicon.ico', 'FaviconData'],
    ['js/core/bootstrap/index.html', 'DeveloperIndexHtmlData'],
    ['js/core/bootstrap/bootstrap.bundle.mjs', 'DeveloperBootstrapData'],
];

export default async function (context) {
    if (context.args.length) throw new Error('Usage: wf run embed-core');
    for (const [source, name] of coreAssets)
        await context.project.write(`include/webfront/system/${name}.hpp`, renderAsset(await context.project.read(source), name));
    context.console.log(`Regenerated ${coreAssets.length} core headers.`);
}
