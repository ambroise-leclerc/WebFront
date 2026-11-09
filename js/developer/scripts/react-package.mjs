// wf run react-package: prepare a publishable npm archive; this never publishes it.
import {packageArchive, writePackage} from './package-archive.mjs';

export const reactPackage = project => packageArchive(project, 'react', 'react');

export default context => writePackage(context, 'react', 'react', 'React adapter');
