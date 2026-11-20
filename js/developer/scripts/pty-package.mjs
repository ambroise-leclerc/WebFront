// wf run pty-package: prepare a publishable npm archive of @webfront/pty.
import {packageArchive, writePackage} from './package-archive.mjs';

export const ptyPackage = project => packageArchive(project, 'pty', 'pty');

export default context => writePackage(context, 'pty', 'pty', 'PTY package');
