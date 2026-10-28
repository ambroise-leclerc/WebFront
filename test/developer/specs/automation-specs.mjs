import {createAutomationRunner,createPreviewInspector} from '../../../js/developer/environment/automation.mjs';
import {exclusive,execute,runNonInteractive} from '../../../js/developer/environment/runtime.mjs';
import {rejects} from '../support.mjs';

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
});
