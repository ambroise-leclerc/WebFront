// wf run services-package: prepare a publishable npm archive of @webfront/services; this never publishes it.
import {packageArchive, writePackage} from './package-archive.mjs';

export const servicesPackage = project => packageArchive(project, 'services', 'services');

export default context => writePackage(context, 'services', 'services', 'Services package');
