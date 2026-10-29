// Uses the declaration that C++ generated from the description of Settings (#383, JourneyDeclaration): wf check verifies
// that the generated types accept the values C++ describes and refuse the others.
import type {Settings} from './generated/settings';

export const defaults: Settings = {speed: 5, density: 0.25, wrap: true, pattern: 'glider', title: 'Life'};

export function summary(settings: Settings): string {
    return `${settings.title}: ${settings.pattern} at speed ${settings.speed}, ${settings.wrap ? 'wrapped' : 'bounded'}`;
}

// @ts-expect-error: a pattern outside the choices of the C++ enumeration.
export const unknownPattern: Settings = {speed: 5, density: 0.25, wrap: true, pattern: 'square', title: 'Life'};
// @ts-expect-error: speed is a number.
export const textSpeed: Settings = {speed: '5', density: 0.25, wrap: true, pattern: 'glider', title: 'Life'};
// @ts-expect-error: a member that C++ does not describe.
export const extra: Settings = {speed: 5, density: 0.25, wrap: true, pattern: 'glider', title: 'Life', colour: 'red'};
// @ts-expect-error: every described member is required.
export const partial: Settings = {speed: 5};
// @ts-expect-error: the generated members are read-only.
defaults.speed = 6;
