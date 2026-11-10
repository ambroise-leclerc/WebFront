// wf check: the declarations of @webfront/services, as installed from the registry.
import {services, selfTest} from '@webfront/services';

const name: '@webfront/services' = selfTest();
const none: Readonly<Record<string, never>> = services;
export {name, none};
