import {clipText,createAutomationRunner,createPreviewInspector,fitCompletion} from '../../../js/developer/environment/automation.mjs';
import {exclusive,execute,runNonInteractive} from '../../../js/developer/environment/runtime.mjs';
import {rejects} from '../support.mjs';
import {memoryBridge} from '../memory-bridge.mjs';
import {setManifestDirectory} from '../../../js/developer/environment/manifest.mjs';
import {withTimers} from '../timers.mjs';

describe('Developer automation', () => {
    it('arbitrates human and agent commands but permits nested wf execution', async () => {
        let release;
        const pending=exclusive({}, async context => {
            expect(await execute(['help'],context)).toContain('wf test');
            await new Promise(resolve => {release=resolve;});
        });
        while (!release) await Promise.resolve();
        await rejects(execute(['help'],{}),/busy/);
        release();
        await pending;
        expect(await execute(['help'],{})).toContain('wf test');
    });

    it('returns a run result and stops cooperatively without granting extra operations', async () => {
        let signal;
        const completed=[];
        const runner=createAutomationRunner({context:{},exclusive,inspector:{},
            run:(_argv,context) => new Promise(resolve => {
                signal=context.signal;
                signal.addEventListener('abort',() => resolve({exitCode:130,output:'stopped',report:null}));
            }),complete:async (id,result) => {completed.push({id,result});}});
        const work=runner.start({runId:'one',operation:'build',arguments:{}});
        runner.cancel('other');
        expect(signal.aborted).toBeFalse();
        runner.cancel('one');
        await work;
        expect(completed[0].result.exitCode).toBe(130);
        await runner.start({runId:'two',operation:'exec',arguments:{}});
        expect(completed[1].result.exitCode).toBe(1);
    });

    it('accepts the next run before the previous native completion acknowledgement returns', async () => {
        let acknowledge, entered;
        const completing = new Promise(resolve => {entered=resolve;});
        const completed=[];
        const runner=createAutomationRunner({context:{},exclusive,inspector:{},
            run:async () => ({exitCode:0,output:'ok',report:null}),
            complete:async id => {
                completed.push(id);
                if (id === 'one') { entered(); await new Promise(resolve => {acknowledge=resolve;}); }
            }});
        const first=runner.start({runId:'one',operation:'build'});
        await completing;
        await runner.start({runId:'two',operation:'check'});
        acknowledge();
        await first;
        expect(completed).toEqual(['one','two']);
    });

    it('rejects a busy start asynchronously and remains reusable', async () => {
        let release;
        const runner=createAutomationRunner({context:{},exclusive,inspector:{},complete:async () => {},
            run:() => new Promise(resolve => {release=() => resolve({exitCode:0,output:'',report:null});})});
        const first=runner.start({runId:'one',operation:'build'});
        let second;
        expect(() => {second=runner.start({runId:'two',operation:'build'});}).not.toThrow();
        await rejects(second,/busy/);
        release();
        await first;
    });

    it('releases an interrupted lease even if the command never resolves, without releasing its successor', async () => {
        const controller=new AbortController();
        let finish, stale;
        const first=exclusive({signal:controller.signal},context => {
            stale=context;
            return new Promise(resolve => {finish=resolve;});
        });
        await withTimers([2500],async clock => {
            controller.abort(new Error('Interrupted'));
            clock.fire(2500);
            await rejects(first,/Interrupted/);
        });
        await exclusive({automation:true},async context => {
            finish();
            await Promise.resolve();
            await rejects(exclusive(stale,async () => {}),/abort|Interrupted/i);
            await rejects(exclusive({},async () => {}),/busy/);
            expect(await execute(['help'],context)).toContain('wf test');
        });
    });

    it('lets agents use a human dev window while its watch is idle', async () => {
        setManifestDirectory();
        const encode=text => new TextEncoder().encode(text);
        const {bridge}=memoryBridge(new Map([['webfront.project.json',encode(JSON.stringify({
            format:1,name:'watch',version:'0.1.0',entries:{main:'main.ts'}
        }))]]));
        const previousWorker=globalThis.Worker;
        globalThis.Worker=class {
            postMessage(message) {
                if (message.type==='build') queueMicrotask(() => this.onmessage({data:{type:'result',files:new Map([['app.js',encode('')]])}}));
            }
            terminate() {}
        };
        const controller=new AbortController();
        let ready, failed;
        const waiting=new Promise((resolve,reject) => {ready=resolve; failed=reject;});
        const preview={hidden:true,contentDocument:null,contentWindow:{location:{replace() {}}},addEventListener() {},removeEventListener() {}};
        const watch=execute(['dev'],{bridge,preview,signal:controller.signal,write:text => {
            if(text.includes('Watching inputs')) ready(); else failed(new Error(text));
        }});
        try {
            await waiting;
            await new Promise(resolve => setTimeout(resolve,0));
            await exclusive({automation:true},async context => expect(await execute(['help'],context)).toContain('wf test'));
        } finally {
            controller.abort();
            await rejects(watch,error => error.exitCode===130);
            if(previousWorker===undefined) delete globalThis.Worker; else globalThis.Worker=previousWorker;
        }
    });

    it('keeps a non-interactive failure structured and the session reusable', async () => {
        const result=await runNonInteractive(['nosuchcommand'],{onExit:() => {}});
        expect(result.exitCode).toBe(1);
        expect(result.output).toContain('Unknown wf command');
        expect((await runNonInteractive(['help'],{onExit:() => {}})).exitCode).toBe(0);
    });

    it('refuses absent previews instead of inspecting the developer terminal', () => {
        const inspector=createPreviewInspector(null);
        expect(() => inspector.inspect()).toThrowError(/No ready application preview/);
        inspector.dispose();
    });

    it('bounds hostile preview text in UTF-8 JSON bytes', () => {
        const doc=document.implementation.createHTMLDocument('preview');
        Object.defineProperty(doc,'URL',{value:location.origin+'/webfront/developer/preview/index.html'});
        Object.defineProperty(doc,'readyState',{value:'complete'});
        for (let index=0;index<100;++index) {
            const element=doc.createElement('button');
            element.id='target-'+index;
            element.textContent='\0'.repeat(128);
            element.setAttribute('aria-label','😀'.repeat(128));
            element.getClientRects=() => [{}];
            doc.body.append(element);
        }
        const frame={hidden:false,contentDocument:doc,addEventListener:() => {},removeEventListener:() => {}};
        const inspector=createPreviewInspector(frame);
        const result=inspector.inspect();
        expect(result.truncated).toBeTrue();
        expect(new TextEncoder().encode(JSON.stringify(result)).length).toBeLessThanOrEqual(30000);
        inspector.dispose();
    });

    it('counts only eligible elements removed by a cap as truncated', () => {
        const doc=document.implementation.createHTMLDocument('preview');
        Object.defineProperty(doc,'URL',{value:location.origin+'/webfront/developer/preview/index.html'});
        Object.defineProperty(doc,'readyState',{value:'complete'});
        for (const tag of ['style','script','div']) {
            const element=doc.createElement(tag);
            element.id='excluded-'+tag;
            if(tag==='div') element.hidden=true;
            doc.body.append(element);
        }
        const frame={hidden:false,contentDocument:doc,addEventListener:() => {},removeEventListener:() => {}};
        const inspector=createPreviewInspector(frame);
        try {
            expect(inspector.inspect().truncated).toBeFalse();
            for(let i=0;i<40;++i) {
                const element=doc.createElement('button'); element.id='target-'+i;
                element.getClientRects=() => [{}]; doc.body.append(element);
            }
            expect(inspector.inspect().elements.length).toBe(40);
            expect(inspector.inspect().truncated).toBeFalse();
            const extra=doc.createElement('button'); extra.id='extra'; extra.getClientRects=() => [{}]; doc.body.append(extra);
            expect(inspector.inspect().truncated).toBeTrue();
        } finally {inspector.dispose();}
    });

    it('cuts text on whole code points so that nothing native code must refuse crosses the bridge', () => {
        const emoji='😀';
        expect(clipText('ab'+emoji,3)).toBe('ab');
        expect(clipText('ab'+emoji,4)).toBe('ab'+emoji);
        expect(clipText(emoji+'ab',3,true)).toBe('ab');
        expect(clipText(emoji+'ab',4,true)).toBe(emoji+'ab');
        expect(clipText(emoji,1)).toBe('');
        expect(clipText('a\ud83dz',3)).toBe('a\ufffdz');
        expect(clipText(undefined,5)).toBe('');
        const cut=clipText(emoji.repeat(10),15);
        expect(JSON.stringify(cut)).not.toMatch(/\\ud[89ab]/i);
    });

    it('reports a native completion failure without keeping the run active', async () => {
        const runner=createAutomationRunner({context:{},exclusive,inspector:{},
            run:async () => ({exitCode:0,output:'x'.repeat(20000),report:null}),
            complete:async () => { throw new Error('refused'); }});
        await rejects(runner.start({runId:'one',operation:'build',arguments:{}}),/refused/);
        expect(runner.output.length).toBeLessThanOrEqual(2048);
        await runner.start({runId:'two',operation:'build',arguments:{}}).catch(() => {});
    });

    it('fits a result of control characters, which JSON escapes to six bytes each, into one bridge string', () => {
        const fitted=fitCompletion({exitCode:1,output:'\u0001'.repeat(16000),report:null});
        expect(new TextEncoder().encode(JSON.stringify(fitted)).length).toBeLessThanOrEqual(56000);
        expect(fitted.truncated).toBeTrue();
        expect(fitCompletion({exitCode:0,output:'ok'}).output).toBe('ok');
    });

    it('reports cancellation as 130 even when the command failed with its own code', async () => {
        const completed=[];
        const runner=createAutomationRunner({context:{},exclusive,inspector:{},
            run:(_argv,context) => new Promise(resolve => context.signal.addEventListener('abort',() => resolve({exitCode:1,output:'Interrupted',report:null}))),
            complete:async (id,result) => {completed.push({id,result});}});
        const work=runner.start({runId:'one',operation:'check',arguments:{}});
        runner.cancel('one');
        await work;
        expect(completed[0].result.exitCode).toBe(130);
    });
});
