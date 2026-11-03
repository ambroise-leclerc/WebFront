// Qualification of <input type="file"> and downloads in the CEF frontend (issue #451): the chooser is answered by the
// client's injection point and opened by a real mouse click sent to the engine; the download goes to a private folder.
#include <webfront/frontend/CEF.hpp>
#include <webfront/system/NativeFS.hpp>
#include <webfront/WebFront.hpp>

#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include <unistd.h>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

struct Scenario {
    std::string           mode;  // download | choose | cancel | dialogs | hold (script dialogs, issue #452)
    std::filesystem::path folder;
    std::filesystem::path fixture;
    std::string           title;
    std::string           downloaded;
    bool                  asked = false;
    std::vector<std::string> dialogs;  // What the injection point saw, in order.
};
Scenario scenario;

class Client : public webfront::cef::SimpleCEFClient {
public:
    Client() : SimpleCEFClient(webfront::detail::BoundContentPolicy{}) {
        chooseFiles = [] {
            scenario.asked = true;
            return scenario.mode == "choose" ? std::vector<std::string>{scenario.fixture.string()} : std::vector<std::string>{};
        };
        if (scenario.mode == "dialogs")
            answerScriptDialog = [](const webfront::frontend::ScriptDialog& dialog) {
                using Kind = webfront::frontend::ScriptDialog::Kind;
                scenario.dialogs.push_back(std::to_string(static_cast<int>(dialog.kind)) + ":" + dialog.message + ":" + dialog.defaultText);
                if (dialog.kind == Kind::prompt)
                    return dialog.message == "name?" ? webfront::frontend::ScriptDialogAnswer{true, "typed"} : webfront::frontend::ScriptDialogAnswer{};
                return webfront::frontend::ScriptDialogAnswer{true, {}};
            };
        downloadDirectory = scenario.folder;
        downloadFinished  = [this](const std::string& path) {
            scenario.downloaded = path;
            finish();
        };
    }
    void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override {
        scenario.title = title.ToString();
        window         = browser;
        if (scenario.title.starts_with("answers:"))
            finish();
        else if (scenario.title == "holding")
            CefPostDelayedTask(TID_UI, new Deadline(this), 1500);  // The engine's own alert is open by then: close under it.
        else if (scenario.title == "ready" && (scenario.mode == "choose" || scenario.mode == "cancel"))
            CefPostDelayedTask(TID_UI, new Click(this, 40), 500);
        else if (scenario.title.starts_with("picked:") || scenario.title == "cancelled")
            finish();
    }
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        SimpleCEFClient::OnAfterCreated(browser);
        window = browser;
        CefPostDelayedTask(TID_UI, new Deadline(this), 20'000);
    }
    void close() {
        if (window)
            window->GetHost()->CloseBrowser(true);
    }
    void finish() {
        if (finishing)
            return;
        finishing = true;
        close();
    }

private:
    CefRefPtr<CefBrowser> window;
    bool                  finishing = false;

public:
    void clickChooser() {
        // Runtime.evaluate with userGesture: the page's own click, counted as a user gesture by the engine.
        auto params = CefDictionaryValue::Create();
        params->SetString("expression", "document.getElementById('picker').click()");
        params->SetBool("userGesture", true);
        window->GetHost()->ExecuteDevToolsMethod(0, "Runtime.evaluate", params);
    }
    // The window may not take input right away: click again until the chooser was requested.
    struct Click : CefTask {
        Click(Client* owner, int left) : client(owner), remaining(left) {}
        void Execute() override {
            if (scenario.asked || remaining == 0)
                return;
            client->clickChooser();
            CefPostDelayedTask(TID_UI, new Click(client.get(), remaining - 1), 250);
        }
        CefRefPtr<Client> client;
        int               remaining;
        IMPLEMENT_REFCOUNTING(Click);
    };
    struct Deadline : CefTask {
        explicit Deadline(Client* owner) : client(owner) {}
        void Execute() override {
            client->close();
        }
        CefRefPtr<Client> client;
        IMPLEMENT_REFCOUNTING(Deadline);
    };
    IMPLEMENT_REFCOUNTING(Client);
};

struct Frontend {
    static constexpr auto action = webfront::frontend::Action::closeServerAfterOpen;
    static void           initialize() {
        webfront::frontend::CEFFrontend::initialize();
    }
    static void open(std::string_view port, std::string_view page) {
        open("127.0.0.1", port, page, {});
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const webfront::ContentPolicy& policy) {
        webfront::cef::detail::openWindow(webfront::frontend::detail::applicationUrl(address, port, page),
                                          webfront::detail::BoundContentPolicy{webfront::detail::applicationOrigin(address, port), policy},
                                          new Client);
    }
};
}  // namespace

int main(int argc, char** argv) {
    try {
        webfront::detail::ensureFrontendInitialized<Frontend>();  // CEF subprocesses run this program too and end here.
        require(argc == 2, "usage: cef_file_services download|choose|cancel|dialogs|hold");
        scenario.mode   = argv[1];
        scenario.folder = std::filesystem::temp_directory_path() / ("webfront-cef-" + scenario.mode + "-" + std::to_string(getpid()));
        std::filesystem::remove_all(scenario.folder);
        std::filesystem::create_directories(scenario.folder);
        scenario.fixture = scenario.folder / "fixture.txt";
        std::ofstream(scenario.fixture) << "chosen by the test";
        using FS = webfront::fs::Multi<webfront::fs::IndexFS, webfront::fs::NativeDebugFS>;
        webfront::BasicWFWithFrontend<webfront::NetProvider, FS, Frontend> app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
        app.show(scenario.mode == "hold"      ? std::string("script-dialogs.html?hold")
                 : scenario.mode == "dialogs" ? std::string("script-dialogs.html")
                                            : "file-services.html?" + (scenario.mode == "download" ? std::string("download") : std::string("choose")));
        if (scenario.mode == "download") {
            require(!scenario.downloaded.empty(), "The download did not finish");
            // The engine neutralizes the page's "../webfront-download.txt" itself (Chrome writes "_webfront-download.txt").
            require(std::filesystem::path(scenario.downloaded).parent_path() == scenario.folder,
                    ("The download left its folder: " + scenario.downloaded).c_str());
            require(std::filesystem::path(scenario.downloaded).filename().string().ends_with("webfront-download.txt"), "The download lost its name");
            std::ifstream stream(scenario.downloaded, std::ios::binary);
            require(stream.is_open(), "The downloaded file could not be opened");
            constexpr std::string_view expected = "downloaded by WebFront";
            std::string                contents(expected.size() + 1, '\0');
            stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
            contents.resize(static_cast<std::size_t>(stream.gcount()));
            require(contents == expected, "The downloaded file has the wrong content");
        } else if (scenario.mode == "hold") {
            // Reaching this point is the check: the window closed under the alert. The engine may release the page's alert
            // while closing, so "released" is as valid as "holding".
            require(scenario.title == "holding" || scenario.title == "released", ("The alert page did not start: " + scenario.title).c_str());
        } else if (scenario.mode == "dialogs") {
            require(scenario.title == "answers:true|typed|null", ("Unexpected answers from the script dialogs: " + scenario.title).c_str());
            const std::vector<std::string> expected{"0:hello:", "1:sure?:", "2:name?:default", "2:cancelled?:x"};
            require(scenario.dialogs == expected, "The injection did not see the expected dialogs");
        } else {
            require(scenario.asked, ("The file chooser was not requested; title=" + scenario.title).c_str());
            require(scenario.title == (scenario.mode == "choose" ? "picked:chosen by the test" : "cancelled"),
                    ("Unexpected page state: " + scenario.title).c_str());
        }
        std::filesystem::remove_all(scenario.folder);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
