// Qualification of <input type="file"> and downloads in the CEF frontend (issue #451): the chooser is answered by the
// client's injection point and opened by a real mouse click sent to the engine; the download goes to a private folder.
#include <webfront/frontend/CEF.hpp>
#include <webfront/system/NativeFS.hpp>
#include <webfront/WebFront.hpp>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include <unistd.h>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

struct Scenario {
    std::string           mode;  // download | choose | cancel
    std::filesystem::path folder;
    std::filesystem::path fixture;
    std::string           title;
    std::string           downloaded;
    bool                  asked = false;
};
Scenario scenario;

class Client : public webfront::cef::SimpleCEFClient {
public:
    Client() : SimpleCEFClient(webfront::detail::BoundContentPolicy{}) {
        chooseFiles = [] {
            scenario.asked = true;
            return scenario.mode == "choose" ? std::vector<std::string>{scenario.fixture.string()} : std::vector<std::string>{};
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
        if (scenario.title == "ready" && scenario.mode != "download")
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
        require(argc == 2, "usage: cef_file_services download|choose|cancel");
        scenario.mode   = argv[1];
        scenario.folder = std::filesystem::temp_directory_path() / ("webfront-cef-" + scenario.mode + "-" + std::to_string(getpid()));
        std::filesystem::remove_all(scenario.folder);
        std::filesystem::create_directories(scenario.folder);
        scenario.fixture = scenario.folder / "fixture.txt";
        std::ofstream(scenario.fixture) << "chosen by the test";
        using FS = webfront::fs::Multi<webfront::fs::IndexFS, webfront::fs::NativeDebugFS>;
        webfront::BasicWFWithFrontend<webfront::NetProvider, FS, Frontend> app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
        app.show("file-services.html?" + (scenario.mode == "download" ? std::string("download") : std::string("choose")));
        if (scenario.mode == "download") {
            require(!scenario.downloaded.empty(), "The download did not finish");
            // The engine neutralizes the page's "../webfront-download.txt" itself (Chrome writes "_webfront-download.txt").
            require(std::filesystem::path(scenario.downloaded).parent_path() == scenario.folder,
                    ("The download left its folder: " + scenario.downloaded).c_str());
            require(std::filesystem::path(scenario.downloaded).filename().string().ends_with("webfront-download.txt"), "The download lost its name");
            std::ifstream stream(scenario.downloaded, std::ios::binary);
            require(std::string(std::istreambuf_iterator<char>(stream), {}) == "downloaded by WebFront", "The downloaded file has the wrong content");
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
