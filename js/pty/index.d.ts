import type { Terminal } from '@xterm/xterm';
import type { FitAddon } from '@xterm/addon-fit';

export interface PtyPageOptions {
    terminal: Terminal;
    fit: FitAddon;
    name?: string;
    webFront?: {
        ready: Promise<unknown>;
        cppFunction(name: string): (...args: (number | Uint8Array)[]) => Promise<unknown>;
    };
    onExit?(code: number): void;
    onError?(error: unknown): void;
    onTruncated?(): void;
}

export interface PtyPage {
    dispose(): void;
    hangUp(): Promise<void>;
}

export declare function mount(element: HTMLElement, options: PtyPageOptions): Promise<PtyPage>;
