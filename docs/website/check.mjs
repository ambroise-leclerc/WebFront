import {readdir,readFile,stat} from 'node:fs/promises';
import {resolve,join} from 'node:path';
import assert from 'node:assert/strict';
import {chapters} from './content.mjs';
import {advance,seed,size} from './dist/assets/life.mjs';
const root=resolve('dist');let pages=0,links=0,bytes=0;
async function walk(dir){for(const entry of await readdir(dir,{withFileTypes:true})){const p=join(dir,entry.name);if(entry.isDirectory()){await walk(p);continue;}bytes+=(await stat(p)).size;if(!p.endsWith('.html')||p.includes('/examples/'))continue;pages++;const html=await readFile(p,'utf8');assert.match(html,/<html lang="(?:fr|en-GB)"/);for(const m of html.matchAll(/(?:href|src)="([^"#]+)(?:#[^"]*)?"/g)){const local=m[1].startsWith('https://webfront.fr/')?new URL(m[1]).pathname:m[1];if(!local.startsWith('/'))continue;let target=join(root,local);const s=await stat(target).catch(()=>null);assert(s,`Missing local target: ${p}: ${m[1]}`);if(s.isDirectory())assert((await stat(join(target,'index.html'))).isFile());links++;}}}
await walk(root);assert.equal(pages,2*(chapters.length+1)+2);
const empty=new Uint8Array(size*size);assert.deepEqual(advance(empty),empty);
const blinker=seed(2);assert.deepEqual(advance(advance(blinker)),blinker);assert.equal(advance(blinker).reduce((a,b)=>a+b),3);
const block=new Uint8Array(size*size);for(const i of [0,31,31*32,1023])block[i]=1;assert.deepEqual(advance(block),block,'Wrapped corners form a stable block');
let glider=seed(1);for(let i=0;i<4;i++)glider=advance(glider);const translated=new Uint8Array(size*size);seed(1).forEach((v,i)=>{if(v)translated[((Math.floor(i/size)+1)%size)*size+(i%size+1)%size]=1;});assert.deepEqual(glider,translated);
console.log(`${pages} pages; ${links} local links/assets checked; ${bytes} bytes; Conway oscillator, toroidal block and glider verified.`);

const sitemap=await readFile(join(root,'sitemap.xml'),'utf8');const locations=[...sitemap.matchAll(/<loc>([^<]+)<\/loc>/g)].map(m=>m[1]);assert.equal(locations.length,2*(chapters.length+1));for(const location of locations){assert.equal(new URL(location).origin,'https://webfront.fr');assert((await stat(join(root,new URL(location).pathname,'index.html'))).isFile());}console.log(`${locations.length} production sitemap locations verified.`);

// Technical names remain readable without admitting raw HTML from editorial content.
const {inline, prose, plain} = await import('./prose.mjs');
assert.match(inline('`std::future<R>`'), /std::future&lt;R&gt;/);
assert.equal(plain('Use `wf test` for a *suite*.'), 'Use wf test for a suite.');
assert(!prose('<script>bad()</script>\n\n- `describe`\n- `it`').includes('<script>'));
for (const language of ['fr','en']) {
    const page = await readFile(join(root,language,'book/tests/index.html'),'utf8');
    const section = page.split('id="part-0"')[1].split('</section>')[0];
    assert.equal((section.match(/<li>/g) || []).length, 5, 'Jasmine functions each need their own list item');
    for (const name of ['describe','it','expect','beforeAll','beforeEach'])
        assert(section.includes(`<code class="inline-code">${name}</code>`), `Unformatted Jasmine name: ${name}`);
    assert(page.includes('id="notation"'), 'Every chapter should explain notation');
}
console.log('Notation, HTML escaping and bilingual Jasmine reference layout verified.');
