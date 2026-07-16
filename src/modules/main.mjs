import { installCppBridge, sendToCpp } from './bridge.mjs';
import { renderDemo } from './ui.mjs';

const root = document.getElementById('root');

if (!root) {
    throw new Error('The module demo root element is missing.');
}

const demo = renderDemo(root, sendToCpp);
installCppBridge(demo.showFromCpp);
