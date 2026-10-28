/// @brief Explicitly bind an automation controller to one developer window. No callbacks in normal applications.
#pragma once
#include "../WebFront.hpp"
#include "Session.hpp"

namespace webfront::automation {
inline constexpr std::string_view connectionScript = R"JS(
void (async () => {
    const call = (name,...args) => webFront.cppFunction('webfront.automation.'+name)(...args);
    try {
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
        runtime.connectAutomation({bridge:webFront,config,preview:document.getElementById('preview')});
    } catch (error) {
        await call('failed',String(error?.message ?? error).slice(0,1024)).catch(() => {});
        window.close();
    }
})();
)JS";

template <typename App>
class Binding {
    using AppUI = typename App::UI;
    struct State {
        std::shared_ptr<Session> session;
        std::mutex               mutex;
        std::optional<AppUI>     browser;
        bool                     closing = false;
        void                     authorize(const AppUI& ui) const {
            if (!browser || browser->id() != ui.id())
                throw std::runtime_error("Automation callback belongs to another page");
        }
    };

public:
    /// Retain until after application shutdown. Only the first connected window is selected.
    Binding(App& app, std::shared_ptr<Session> session) : state(std::make_shared<State>()) {
        state->session   = std::move(session);
        const auto owned = state;
        app.cppFunction("webfront.automation.poll", [owned](AppUI ui, std::string output) {
            std::scoped_lock lock{owned->mutex};
            owned->authorize(ui);
            return owned->session->poll(std::move(output));
        });
        app.cppFunction("webfront.automation.complete", [owned](AppUI ui, std::string id, std::string result) {
            std::scoped_lock lock{owned->mutex};
            owned->authorize(ui);
            owned->session->complete(id, std::move(result));
        });
        app.cppFunction("webfront.automation.failed", [owned](AppUI ui, std::string reason) {
            std::scoped_lock lock{owned->mutex};
            owned->authorize(ui);
            owned->session->close(reason.substr(0, 1024));
        });
        started = app.subscribeUIStarted([owned](AppUI ui) {
            std::scoped_lock lock{owned->mutex};
            if (owned->browser)
                return;
            owned->browser.emplace(ui);
            ui.addScript(owned->closing ? "window.close()" : connectionScript);
        });
        closed  = app.subscribeUIClosed([owned](AppUI ui) {
            std::scoped_lock lock{owned->mutex};
            if (owned->browser && owned->browser->id() == ui.id())
                owned->session->close("Developer window disconnected");
        });
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
