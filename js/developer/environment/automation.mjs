// Optional developer-window automation. Native code chooses the page and grants operations.
// This module neither grants permissions nor exports arbitrary callbacks, eval or a shell.
import {fitResult} from './testing.mjs';
const parseMessage=JSON.parse.bind(JSON), serializeMessage=JSON.stringify.bind(JSON);

/** Cuts text to at most `max` UTF-16 units, keeping its start or (fromEnd) its end, without splitting a surrogate pair:
 *  a lone surrogate cannot cross the bridge, and native code would refuse the whole message. */
export function clipText(value, max, fromEnd = false) {
    let text = String(value ?? '');
    if (text.length > max) {
        if (fromEnd) {
            let start = text.length - max;
            if ((text.charCodeAt(start) & 0xFC00) === 0xDC00) ++start;
            text = text.slice(start);
        } else {
            let end = max;
            if (end > 0 && (text.charCodeAt(end - 1) & 0xFC00) === 0xD800) --end;
            text = text.slice(0, end);
        }
    }
    return text.toWellFormed?.() ?? text;
}
export function createPreviewInspector(frame) {
    let revision = 0, observed = null, navigating = false, errorsTruncated = false;
    const errors = [];
    let detachErrors = () => {};
    const loaded = () => {
        ++revision; navigating=false; errors.length=0; errorsTruncated=false; detachErrors();
        try {
            const view=frame.contentWindow;
            if (view.location.origin !== location.origin) return;
            const error = event => {
                const text=String(event.message ?? event.reason ?? 'Preview error');
                errorsTruncated ||= errors.length>=20 || text.length>512;
                if (errors.length<20) errors.push(clipText(text,512));
            };
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
            let truncated = errorsTruncated;
            const field = (value,max) => { const text=String(value); truncated ||= text.length>max; return clipText(text,max); };
            // Explicit IDs give stable semantic targets. Do not export form values or full HTML.
            for (const element of doc.querySelectorAll('[id]')) {
                if (element.closest('script,style,[hidden],[aria-hidden="true"],[contenteditable]')) continue;
                if (!element.getClientRects().length) continue;
                if (elements.length === 40) { truncated = true; break; }
                const form = /^(INPUT|TEXTAREA|SELECT)$/.test(element.tagName);
                elements.push({id: field(element.id,256), tag: element.tagName.toLowerCase(),
                    role: field(element.getAttribute('role') ?? '',64),
                    text: form ? '' : field([...element.childNodes].filter(node => node.nodeType === 3).map(node => node.textContent).join(''),128),
                    label: field(element.getAttribute('aria-label') ?? '',128)});
            }
            const title=field(doc.title,256);
            const result={pageId, ready:true, title, elements, errors:[...errors], truncated};
            const encoder=new TextEncoder();
            while (encoder.encode(serializeMessage(result)).length>30000) {
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

const resultBytes = 56000;  // under the 60,000-byte native message limit and the bridge's 16-bit string length
/** Shrinks a result's output until its JSON, which escapes control characters to six bytes each, fits one bridge string. */
export function fitCompletion(result) {
    const encoder = new TextEncoder();
    const size = value => encoder.encode(serializeMessage(value)).length;
    const output = String(result.output ?? '');
    result = {...result, output: clipText(output,16000,true), ...(output.length>16000 ? {truncated:true} : {})};
    if (size(result)>resultBytes && result.report) {
        const report=fitResult({...result,output:''},resultBytes/2);
        result={...report,output:result.output,truncated:true};
    }
    while (size(result) > resultBytes && result.output.length) result = {...result, output: clipText(result.output, result.output.length >> 1,true), truncated: true};
    return size(result) > resultBytes ? {exitCode: result.exitCode, output: 'The result is too large to report', truncated: true} : result;
}

/** One active native run, cooperatively cancellable; poll stays responsive while a command runs. */
export function createAutomationRunner({run, exclusive, context, inspector, onState = () => {}, complete}) {
    let active = null, output = '';
    const clip = value => clipText(value,16000,true);
    return {
        get output() { return clipText(output,2048,true); },
        cancel(id) { if (active?.id === id) active.controller.abort(new Error('Interrupted by automation')); },
        async start(request) {
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
                        result = await run(argv, {...context, signal:controller.signal, onExit:() => {}, outputLimit:16000,
                            write:text => { output = clip(output + text); }, reporter:undefined});
                        // A command that stopped because of the cancellation reports it, whatever code it chose.
                        if (controller.signal.aborted && result.exitCode !== 0) result = {...result, exitCode:130};
                    }
                } catch (error) { result = {exitCode:controller.signal.aborted ? 130 : 1, output:clip(error?.message ?? error), report:null}; }
                result = fitCompletion(result);
                output = clip(result.output);
                // Native completion permits the next run immediately, even before its bridge reply returns.
                // Release local ownership first so polling can accept that run during the round trip.
                if (active === owned) active = null;
                onState(`${request.operation}: ${result.exitCode === 0 ? 'succeeded' : 'failed'} (${request.runId})`);
                await complete(request.runId, result);
                return result;
            })();
            return finished;
        },
        stop() { active?.controller.abort(new Error('Automation session closed')); }
    };
}

/** `call(name, ...args)` invokes a native automation callback; the page that connects holds its capability. */
export function connectAutomation({bridge, call, preview, run, exclusive, ...context}) {
    if (typeof call !== 'function') throw new Error('Automation needs the page capability to call native code');
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
        onState:text => {status.textContent=text;}, complete:(id,result) => call('complete',id,serializeMessage(result))});
    let stopped = false, timer, activeId;
    stop.addEventListener('click', () => { if (activeId) runner.cancel(activeId); });
    const close = () => { stopped=true; clearTimeout(timer); runner.stop(); inspector.dispose(); panel.remove(); };
    const poll = async () => {
        try {
            const request = parseMessage(await call('poll',runner.output));
            if (request?.close) { close(); globalThis.close(); return; }
            if (request?.cancel) runner.cancel(request.cancel);
            else if (request?.runId) {
                activeId = request.runId;
                // Completion is always observed, including disconnect/revocation reporting failures.
                void runner.start(request).catch(error => {status.textContent=String(error?.message ?? error);});
            }
        } catch (error) {
            status.textContent=`Automation disconnected: ${error?.message ?? error}`;
            await call('failed',clipText(error?.message ?? error,1024)).catch(() => {});
            close(); globalThis.close(); return;
        }
        if (!stopped) timer=setTimeout(poll,50);
    };
    void poll();
    return close;
}
