#include <webfront/frontend/CEF.hpp>
#include <webfront/system/NativeFS.hpp>
#include <webfront/WebFront.hpp>

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

std::vector<std::string> openedPopups;
void                     recordPopup(const std::string& url) {
    openedPopups.push_back(url);
}

void checkPopupGestures() {
    openedPopups.clear();
    CefRefPtr<webfront::cef::SimpleCEFClient> client = new webfront::cef::SimpleCEFClient(
        webfront::detail::BoundContentPolicy{"http://127.0.0.1:9002", {.popups = webfront::PopupPolicy::systemBrowser}},
        recordPopup);
    const std::string             url = "https://example.com/help";
    CefPopupFeatures              features;
    CefWindowInfo                 info;
    CefRefPtr<CefClient>          popupClient;
    CefBrowserSettings            settings;
    CefRefPtr<CefDictionaryValue> extraInfo;
    bool                          noJavascriptAccess = false;
    for (const bool gesture : {false, true}) {
        require(client->OnBeforePopup(nullptr,
                                      nullptr,
                                      0,
                                      url,
                                      "",
                                      CEF_WOD_NEW_FOREGROUND_TAB,
                                      gesture,
                                      features,
                                      info,
                                      popupClient,
                                      settings,
                                      extraInfo,
                                      &noJavascriptAccess),
                "Unmanaged page popup was allowed");
        require(client->OnOpenURLFromTab(nullptr, nullptr, url, CEF_WOD_NEW_FOREGROUND_TAB, gesture), "Unmanaged new tab was allowed");
    }
    require(openedPopups == std::vector<std::string>{url, url}, "Only user-initiated CEF popups and new tabs should launch externally");
}

struct Probe {
    bool               devToolsPopupHook         = false;
    bool               devToolsClientIsolated    = false;
    bool               devToolsOpened            = false;
    bool               devToolsClosed            = false;
    bool               parentNavigatedAfterClose = false;
    std::exception_ptr failure;
};

class DevToolsClient : public CefClient, public CefLifeSpanHandler {
public:
    DevToolsClient(Probe& state, CefRefPtr<CefBrowser> opener, CefRefPtr<CefClient> client)
        : probe(state), parent(std::move(opener)), delegate(std::move(client)) { }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override {
        return delegate->GetRequestHandler();
    }
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        if (auto handler = delegate->GetLifeSpanHandler())
            handler->OnAfterCreated(browser);
        probe.devToolsOpened = true;
        browser->GetHost()->CloseBrowser(true);
    }
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        if (auto handler = delegate->GetLifeSpanHandler())
            handler->OnBeforeClose(browser);
        probe.devToolsClosed = true;
        auto frame           = parent->GetMainFrame();
        frame->LoadURL(frame->GetURL().ToString() + "?after-devtools-close");
    }

private:
    Probe&                probe;
    CefRefPtr<CefBrowser> parent;
    CefRefPtr<CefClient>  delegate;
    IMPLEMENT_REFCOUNTING(DevToolsClient);
};

class MenuDelegate : public CefMenuModelDelegate {
public:
    void ExecuteCommand(CefRefPtr<CefMenuModel>, int, cef_event_flags_t) override { }

private:
    IMPLEMENT_REFCOUNTING(MenuDelegate);
};

class PolicyClient : public webfront::cef::SimpleCEFClient {
public:
    // Deliberately start with different settings: the window runner must bind its passed policy.
    explicit PolicyClient(Probe& state)
        : SimpleCEFClient(webfront::detail::BoundContentPolicy{
              "http://127.0.0.1:9002",
              {.devToolsEnabled = false, .contextMenusEnabled = false}
    }),
          probe(state) { }
    void OnBeforeDevToolsPopup(CefRefPtr<CefBrowser>          browser,
                               CefWindowInfo&                 info,
                               CefRefPtr<CefClient>&          client,
                               CefBrowserSettings&            settings,
                               CefRefPtr<CefDictionaryValue>& extraInfo,
                               bool*                          useDefaultWindow) override {
        probe.devToolsPopupHook = true;
        SimpleCEFClient::OnBeforeDevToolsPopup(browser, info, client, settings, extraInfo, useDefaultWindow);
        probe.devToolsClientIsolated = client && client.get() != this && !client->GetRequestHandler() && !client->GetLifeSpanHandler();
        // Observe closure while retaining the production client's navigation and lifecycle handlers.
        client = new DevToolsClient(probe, browser, client);
    }
    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int status) override {
        SimpleCEFClient::OnLoadEnd(browser, frame, status);
        if (!frame->IsMain())
            return;
        if (probe.devToolsClosed && frame->GetURL().ToString().ends_with("?after-devtools-close")) {
            probe.parentNavigatedAfterClose = true;
            browser->GetHost()->CloseBrowser(true);
            return;
        }
        if (started)
            return;
        started = true;
        try {
            checkControls();
            checkPopupGestures();
            CefWindowInfo      info;
            CefBrowserSettings settings;
            browser->GetHost()->ShowDevTools(info, nullptr, settings, CefPoint{});
        } catch (...) {
            probe.failure = std::current_exception();
            browser->GetHost()->CloseBrowser(true);
        }
    }

private:
    Probe& probe;
    bool   started = false;

    void checkControls() {
        using webfront::cef::SimpleCEFClient;
        using webfront::detail::BoundContentPolicy;
        CefRefPtr<SimpleCEFClient> disabled = new SimpleCEFClient(BoundContentPolicy{"http://127.0.0.1:9002", {.devToolsEnabled = false}});
        for (const auto* name : {"IDC_DEV_TOOLS",
                                 "IDC_DEV_TOOLS_CONSOLE",
                                 "IDC_DEV_TOOLS_DEVICES",
                                 "IDC_DEV_TOOLS_INSPECT",
                                 "IDC_DEV_TOOLS_TOGGLE",
                                 "IDC_CONTENT_CONTEXT_INSPECTELEMENT",
                                 "IDC_CONTENT_CONTEXT_INSPECTELEMENT_WITH_DEVTOOLS"}) {
            const int id = cef_id_for_command_id_name(name);
            require(id >= 0, name);
            require(!OnChromeCommand(nullptr, id, CEF_WOD_CURRENT_TAB), "Enabled DevTools command was blocked");
            require(disabled->OnChromeCommand(nullptr, id, CEF_WOD_CURRENT_TAB), "Disabled DevTools command was allowed");
        }
        const int inspect          = cef_id_for_command_id_name("IDC_CONTENT_CONTEXT_INSPECTELEMENT");
        const int inspectWithTools = cef_id_for_command_id_name("IDC_CONTENT_CONTEXT_INSPECTELEMENT_WITH_DEVTOOLS");
        auto      menu             = CefMenuModel::CreateMenuModel(new MenuDelegate);
        require(menu != nullptr, "Menu model creation failed");
        menu->AddItem(MENU_ID_COPY, "Copy");
        menu->AddItem(inspect, "Inspect");
        menu->AddItem(inspectWithTools, "Inspect with DevTools");
        OnBeforeContextMenu(nullptr, nullptr, nullptr, menu);
        require(menu->GetCount() == 3, "Default context menu was changed");
        disabled->OnBeforeContextMenu(nullptr, nullptr, nullptr, menu);
        require(menu->GetCount() == 1 && menu->GetIndexOf(MENU_ID_COPY) == 0, "Inspect entries were not filtered");
        CefRefPtr<SimpleCEFClient> noMenus = new SimpleCEFClient(BoundContentPolicy{"http://127.0.0.1:9002", {.contextMenusEnabled = false}});
        noMenus->OnBeforeContextMenu(nullptr, nullptr, nullptr, menu);
        require(menu->GetCount() == 0, "Disabled context menu was not cleared");
    }
};

struct ProbeFrontend {
    static constexpr auto action = webfront::frontend::Action::closeServerAfterOpen;
    static void           initialize() {
        webfront::frontend::CEFFrontend::initialize();
    }
    static void open(std::string_view port, std::string_view page) {
        open("127.0.0.1", port, page, {});
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const webfront::ContentPolicy& policy) {
        Probe probe;
        // Exercise the internal window runner with a client observing the real engine callbacks.
        webfront::cef::detail::openWindow(webfront::frontend::detail::applicationUrl(address, port, page),
                                          webfront::detail::BoundContentPolicy{webfront::detail::applicationOrigin(address, port), policy},
                                          new PolicyClient(probe));
        if (probe.failure)
            std::rethrow_exception(probe.failure);
        require(probe.devToolsPopupHook, "CEF did not use OnBeforeDevToolsPopup");
        require(probe.devToolsClientIsolated, "DevTools inherited application policy or lifetime handlers");
        require(probe.devToolsOpened, "Default popup blocking prevented DevTools creation");
        require(probe.devToolsClosed, "DevTools did not close");
        require(probe.parentNavigatedAfterClose, "Closing DevTools ended the application's message loop");
    }
};
}  // namespace

int main() {
    try {
        using FS = webfront::fs::Multi<webfront::fs::IndexFS, webfront::fs::NativeDebugFS>;
        webfront::BasicWFWithFrontend<webfront::NetProvider, FS, ProbeFrontend> app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
        app.show("policy-controls.html");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
