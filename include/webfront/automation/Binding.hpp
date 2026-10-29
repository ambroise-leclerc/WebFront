/// @brief Explicitly bind an automation controller to one developer window. No callbacks in normal applications.
#pragma once
#include "../WebFront.hpp"
#include "PageAuthentication.hpp"

namespace webfront::automation {
// The bootstrap secret is removed from the DOM before yielding. Later packets carry a one-use MAC, never that secret.
inline constexpr std::string_view connectionScriptTemplate = R"JS(
void (async () => {
    document.currentScript?.remove();
    const encode = TextEncoder.prototype.encode.bind(new TextEncoder());
    const parse = JSON.parse.bind(JSON);
    const sign = crypto.subtle.sign.bind(crypto.subtle);
    const verify = crypto.subtle.verify.bind(crypto.subtle);
    let key;
    const cppFunction = webFront.cppFunction.bind(webFront);
    const payload = (sequence,method,args) => {
        let value='webfront.automation.v1\n'+sequence+'\n'+method+'\n';
        for (const argument of args) value+=encode(argument).length+':'+argument;
        return encode(value);
    };
    let sequence=0n, pending=Promise.resolve();
    const call = (name,...args) => {
        const next=pending.then(async () => {
            const id=''+(++sequence);
            const bytes=new Uint8Array(await sign('HMAC',key,payload(id,name,args)));
            let mac=''; for(const byte of bytes) mac+=byte.toString(16).padStart(2,'0');
            const reply=parse(await cppFunction('webfront.automation.'+name)(id,mac,...args));
            if(typeof reply.value!=='string' || !/^[0-9a-f]{64}$/.test(reply.mac)) throw new Error('Invalid authenticated reply');
            const proof=new Uint8Array(32);
            for(let i=0;i<32;++i) proof[i]=parseInt(reply.mac.slice(i*2,i*2+2),16);
            if(!await verify('HMAC',key,proof,payload(id,'reply:'+name,[reply.value]))) throw new Error('Invalid authenticated reply');
            return reply.value;
        });
        pending=next.catch(() => {});
        return next;
    };
    try {
        key = await crypto.subtle.importKey('raw',encode('__CAPABILITY__'),{name:'HMAC',hash:'SHA-256'},false,['sign','verify']);
        await webFront.ready;
        await new Promise((resolve,reject) => {
            const observer = new MutationObserver(check);
            function check() {
                const text = document.getElementById('status')?.textContent ?? '';
                if (text.startsWith('Bootstrap failed:')) { observer.disconnect(); reject(new Error(text)); }
                else if (text.startsWith('Developer environment ready.')) { observer.disconnect(); resolve(); }
            }
            observer.observe(document.documentElement,{subtree:true,childList:true,characterData:true});
            check();
        });
        const config = JSON.parse(await webFront.cppFunction('webfront.developer.config')());
        const response = await fetch('/webfront/developer/environment/environment.json');
        if (!response.ok) throw new Error('Cannot read developer environment metadata');
        const environment = await response.json();
        const runtime = await import('/webfront/developer/environment/'+environment.entry);
        if (typeof runtime.connectAutomation !== 'function') throw new Error('This environment has no automation API v1; provision an updated environment');
        runtime.connectAutomation({bridge:webFront,call,config,preview:document.getElementById('preview')});
    } catch (error) {
        await call('failed',String(error?.message ?? error).slice(0,1024)).catch(() => {});
        window.close();
    }
})();
)JS";

inline std::string connectionScript(std::string_view capability) {
    std::string script{connectionScriptTemplate};
    script.replace(script.find("__CAPABILITY__"), std::string_view("__CAPABILITY__").size(), capability);
    return script;
}

template <typename App>
class Binding {
    using AppUI = typename App::UI;
    struct State {
        std::shared_ptr<Session>           session;
        std::mutex                         mutex;
        std::optional<AppUI>               browser;
        authentication::PageAuthentication pageAuthentication;
        std::string                        launchToken = channel::nonce();
        bool                               closing     = false;
        void
        authorize(const AppUI& ui, std::string_view sequence, std::string_view mac, std::string_view method, std::initializer_list<std::string_view> args) {
            if (!browser || browser->id() != ui.id())
                throw std::runtime_error("Automation callback belongs to another page");
            pageAuthentication.authorize(sequence, mac, method, args);
        }
        std::string reply(std::string_view sequence, std::string_view method, std::string value) const {
            return objectJson({
                {"value", json::quote(value)},
                {"mac",
                 json::quote(authentication::hmac(pageAuthentication.secret(), authentication::payload(sequence, "reply:" + std::string(method), {value})))}
            });
        }
    };

public:
    /// Retain until after application shutdown. Only a browser proving the launch URL's secret is selected.
    Binding(App& app, std::shared_ptr<Session> session) : state(std::make_shared<State>()) {
        state->session   = std::move(session);
        const auto owned = state;
        app.cppFunction("webfront.automation.claim", [owned](AppUI ui, std::string token) {
            std::scoped_lock lock{owned->mutex};
            if (owned->browser || !channel::sameToken(token, owned->launchToken))
                throw std::runtime_error("Automation window claim rejected");
            owned->browser.emplace(ui);
            owned->launchToken.clear();  // one successful claim; later connections cannot replace the selected browser
            ui.addScript(owned->closing ? std::string{"window.close()"} : connectionScript(owned->pageAuthentication.secret()));
            return std::string{"null"};
        });
        app.cppFunction("webfront.automation.poll", [owned](AppUI ui, std::string sequence, std::string mac, std::string output) {
            std::scoped_lock lock{owned->mutex};
            owned->authorize(ui, sequence, mac, "poll", {output});
            return owned->reply(sequence, "poll", owned->session->poll(std::move(output)));
        });
        app.cppFunction("webfront.automation.complete", [owned](AppUI ui, std::string sequence, std::string mac, std::string id, std::string result) {
            std::scoped_lock lock{owned->mutex};
            owned->authorize(ui, sequence, mac, "complete", {id, result});
            owned->session->complete(id, std::move(result));
            return owned->reply(sequence, "complete", "null");
        });
        app.cppFunction("webfront.automation.failed", [owned](AppUI ui, std::string sequence, std::string mac, std::string reason) {
            std::scoped_lock lock{owned->mutex};
            owned->authorize(ui, sequence, mac, "failed", {reason});
            owned->session->close(reason.substr(0, 1024));
            return owned->reply(sequence, "failed", "null");
        });
        started = app.subscribeUIStarted([owned](AppUI ui) {
            std::scoped_lock lock{owned->mutex};
            if (owned->browser)
                return;
            // The fragment is delivered only to the launched frontend, never in HTTP/WebSocket requests.
            // A connection racing the real window gets no page key and cannot consume the claim.
            ui.addScript(R"JS(void (async () => {
                const prefix='#webfront-automation=';
                if (!location.hash.startsWith(prefix)) return;
                const token=location.hash.slice(prefix.length);
                history.replaceState(null,'',location.pathname+location.search);
                try { await webFront.ready; await webFront.cppFunction('webfront.automation.claim')(token); }
                catch { window.close(); }
            })();)JS");
        });
        closed  = app.subscribeUIClosed([owned](AppUI ui) {
            std::scoped_lock lock{owned->mutex};
            if (owned->browser && owned->browser->id() == ui.id())
                owned->session->close("Developer window disconnected");
        });
    }
    std::string launchPage() const {
        std::scoped_lock lock{state->mutex};
        return "webfront-bootstrap.html#webfront-automation=" + state->launchToken;
    }
    void closeBrowser() {
        std::scoped_lock lock{state->mutex};
        state->closing = true;
        if (state->browser) {
            try {
                state->browser->addScript("window.close()");
            } catch (const std::exception&) { }  // a native watchdog still supervises renderer termination
        }
    }

private:
    std::shared_ptr<State> state;
    UISubscription         started, closed;
};
}  // namespace webfront::automation
