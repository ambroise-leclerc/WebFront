// Optional developer-window automation. Native code chooses the page and grants operations.
// This module neither grants permissions nor exports arbitrary callbacks, eval or a shell.
export function createPreviewInspector(frame) {
    let revision = 0, observed = null, navigating = false;
    const errors = [];
    let detachErrors = () => {};
    const loaded = () => {
        ++revision; navigating=false; errors.length=0; detachErrors();
        try {
            const view=frame.contentWindow;
            if (view.location.origin !== location.origin) return;
            const error = event => { if (errors.length<20) errors.push(String(event.message ?? event.reason ?? 'Preview error').slice(0,512)); };
            view.addEventListener('error',error);
            view.addEventListener('unhandledrejection',error);
            detachErrors=() => {view.removeEventListener('error',error);view.removeEventListener('unhandledrejection',error);};
        } catch { /* Cross-origin frames are refused by page(), without reading their content. */ }
    };
    const navigation = () => { navigating=true; ++revision; };
    frame?.addEventListener('load', loaded);
    frame?.addEventListener('webfront-preview-navigating', navigation);
    const page = () => {
        if (!frame || frame.hidden || !frame.contentDocument || navigating) throw new Error('No ready application preview');
        const doc = frame.contentDocument;
        const url = new URL(doc.URL);
        if (url.origin !== location.origin || !url.pathname.startsWith('/webfront/developer/preview/'))
            throw new Error('The preview is outside its authorized origin/path');
        if (doc !== observed) { observed = doc; ++revision; }
        if (doc.readyState !== 'complete') throw new Error('Application preview is loading');
        return {doc, pageId: String(revision)};
    };
    return {
        inspect() {
            const {doc, pageId} = page();
            const elements = [];
            // Explicit IDs give stable semantic targets. Do not export form values or full HTML.
            for (const element of doc.querySelectorAll('[id]')) {
                if (elements.length === 40) break;
                if (element.closest('script,style,[hidden],[aria-hidden="true"],[contenteditable]')) continue;
                if (!element.getClientRects().length) continue;
                const form = /^(INPUT|TEXTAREA|SELECT)$/.test(element.tagName);
                elements.push({id: element.id.slice(0,256), tag: element.tagName.toLowerCase(),
                    role: (element.getAttribute('role') ?? '').slice(0,64),
                    text: form ? '' : [...element.childNodes].filter(node => node.nodeType === 3).map(node => node.textContent).join('').slice(0,128),
                    label: (element.getAttribute('aria-label') ?? '').slice(0,128)});
            }
            const result={pageId, ready:true, title:doc.title.slice(0,256), elements, errors:[...errors], truncated:doc.querySelectorAll('[id]').length > elements.length};
            const encoder=new TextEncoder();
            while (encoder.encode(JSON.stringify(result)).length>30000) {
                if (result.errors.length) result.errors.pop();
                else if (result.elements.length) result.elements.pop();
                else break;
                result.truncated=true;
            }
            return result;
        },
        action(operation, {pageId, elementId, value}) {
            const current = page();
            if (pageId !== current.pageId) throw new Error('Preview changed; inspect again before acting');
            const element = current.doc.getElementById(elementId);
            if (!element || element.hidden || element.disabled || !element.getClientRects().length)
                throw new Error('Target is absent, hidden or disabled');
            if (operation === 'click') {
                if (!/^(BUTTON|INPUT)$/.test(element.tagName) || element.tagName === 'INPUT' && !['button','checkbox','radio','submit'].includes(element.type))
                    throw new Error('Only buttons, checkboxes and radio controls can be clicked');
                element.click();
            } else {
                if (!/^(INPUT|TEXTAREA)$/.test(element.tagName) || element.tagName === 'INPUT' && !['text','search','email','url','tel','number'].includes(element.type))
                    throw new Error('Only ordinary text and number controls can be filled');
                if (element.readOnly) throw new Error('Target is read-only');
                const view = current.doc.defaultView;
                const prototype = element.tagName === 'INPUT' ? view.HTMLInputElement.prototype : view.HTMLTextAreaElement.prototype;
                Object.getOwnPropertyDescriptor(prototype, 'value').set.call(element, value);
                element.dispatchEvent(new view.Event('input', {bubbles:true}));
                element.dispatchEvent(new view.Event('change', {bubbles:true}));
            }
            return {pageId:current.pageId, elementId, applied:true};
        },
        dispose() { frame?.removeEventListener('load', loaded); frame?.removeEventListener('webfront-preview-navigating',navigation); detachErrors(); }
    };
}

/** One active native run, cooperatively cancellable; poll stays responsive while a command runs. */
export function createAutomationRunner({run, exclusive, context, inspector, onState = () => {}, complete}) {
    let active = null, output = '';
    const clip = value => String(value).slice(0,16000);
    return {
        get output() { return output.slice(-2048); },
        cancel(id) { if (active?.id === id) active.controller.abort(new Error('Interrupted by automation')); },
        start(request) {
            if (active) throw new Error('Automation runner is busy');
            const controller = new AbortController();
            const owned = active = {id:request.runId, controller};
            output = '';
            onState(`Running ${request.operation} (${request.runId})`);
            const finished = (async () => {
                let result;
                try {
                    const args = request.arguments ?? {};
                    if (['inspect','click','fill'].includes(request.operation)) {
                        result = await exclusive(context, async () => ({exitCode:0, output:'', report:null,
                            data:request.operation === 'inspect' ? inspector.inspect() : inspector.action(request.operation,args)}));
                    } else {
                        if (!['check','build','test','preview'].includes(request.operation)) throw new Error('Unknown automation operation');
                        const argv = [request.operation];
                        if (args.entry) argv.push(args.entry);
                        if (args.pattern) argv.push(args.pattern);
                        result = await run(argv, {...context, signal:controller.signal, onExit:() => {},
                            write:text => { output = clip(output + text); }, reporter:undefined});
                    }
                } catch (error) { result = {exitCode:controller.signal.aborted ? 130 : 1, output:clip(error?.message ?? error), report:null}; }
                output = clip(result.output);
                try { await complete(request.runId, result); }
                finally {
                    if (active === owned) active = null;
                    onState(`${request.operation}: ${result.exitCode === 0 ? 'succeeded' : 'failed'} (${request.runId})`);
                }
                return result;
            })();
            return finished;
        },
        stop() { active?.controller.abort(new Error('Automation session closed')); }
    };
}

export function connectAutomation({bridge, preview, run, exclusive, ...context}) {
    const call = (name,...args) => bridge.cppFunction('webfront.automation.'+name)(...args);
    const panel = document.createElement('section');
    panel.setAttribute('aria-label','Developer automation');
    const status = document.createElement('output');
    status.textContent = 'Automation connected';
    const stop = document.createElement('button');
    stop.textContent = 'Stop automation operation';
    panel.append(status,stop);
    document.querySelector('header')?.append(panel);
    const inspector = createPreviewInspector(preview);
    const runner = createAutomationRunner({run, exclusive, context:{...context,bridge,preview,automation:true}, inspector,
        onState:text => {status.textContent=text;}, complete:(id,result) => call('complete',id,JSON.stringify(result))});
    let stopped = false, timer, activeId;
    stop.addEventListener('click', () => { if (activeId) runner.cancel(activeId); });
    const close = () => { stopped=true; clearTimeout(timer); runner.stop(); inspector.dispose(); panel.remove(); };
    const poll = async () => {
        try {
            const request = JSON.parse(await call('poll',runner.output));
            if (request?.close) { close(); globalThis.close(); return; }
            if (request?.cancel) runner.cancel(request.cancel);
            else if (request?.runId) {
                activeId = request.runId;
                // Completion is always observed, including disconnect/revocation reporting failures.
                void runner.start(request).catch(error => {status.textContent=String(error?.message ?? error);});
            }
        } catch (error) { status.textContent=`Automation disconnected: ${error?.message ?? error}`; close(); return; }
        if (!stopped) timer=setTimeout(poll,50);
    };
    void poll();
    return close;
}
