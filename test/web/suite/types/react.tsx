import React from 'react';
import {Island, createStore, useStore, type DomInterpreter} from '@webfront/react';
const store = createStore<[number, string]>('typed', [0, 'initial']);
declare const dom: DomInterpreter;
export function Panel() {
    const [count, label] = useStore(store);
    // @ts-expect-error -- tuple fields retain their types
    const wrong: string = count;
    return <section>{label}: {count}<Island dom={dom} name="panel" aria-label="C++ panel"/></section>;
}
// @ts-expect-error -- C++ owns the children
const children = <Island dom={dom} name="panel">forbidden</Island>;
// @ts-expect-error -- HTML cannot bypass the DOM ownership boundary
const html = <Island dom={dom} name="panel" dangerouslySetInnerHTML={{__html: 'forbidden'}}/>;
store.dispose();
