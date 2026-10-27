import type {HTMLAttributes, ReactElement} from 'react';
export interface Store<T> {
    getSnapshot(): T;
    subscribe(listener: () => void): () => void;
    dispose(): void;
}
export declare function createStore<T>(name: string, initialSnapshot: T, options?: {target?: typeof globalThis | Window}): Store<T>;
export declare function useStore<T>(store: Store<T>): T;
export interface DomInterpreter {
    island(name: string, element: HTMLElement): () => void;
}
export type IslandProps = Omit<HTMLAttributes<HTMLDivElement>, 'children' | 'dangerouslySetInnerHTML'> & {
    dom: DomInterpreter;
    name: string;
    children?: never;
    dangerouslySetInnerHTML?: never;
};
export declare function Island(props: IslandProps): ReactElement;
