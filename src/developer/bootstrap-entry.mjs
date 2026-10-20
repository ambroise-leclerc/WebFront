import {bootstrap, call} from './bootstrap.mjs';

if (typeof document !== 'undefined' && document.getElementById('retry')) {
    const status = document.getElementById('status');
    const retry = document.getElementById('retry');
    const start = async () => {
        retry.hidden = true;
        try {
            await webFront.ready;
            const config = JSON.parse(await call(webFront, 'config'));
            const {url} = await bootstrap(webFront, config, fetch, text => { status.textContent = text; });
            const environment = await import(url);
            await environment.mount({bridge: webFront, config, terminal: document.getElementById('terminal'), preview: document.getElementById('preview')});
            status.textContent = 'Developer environment ready. Type wf help.';
        } catch (error) {
            status.textContent = `Bootstrap failed: ${error.message}`;
            retry.hidden = false;
        }
    };
    retry.addEventListener('click', start);
    void start();
}
