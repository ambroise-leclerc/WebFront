// ADR-017 (#363): measures the HTML bridge (WebSocket to the embedded server) against WebView2's own message channel, in
// the same window and with the same echo calls (transport-benchmark.html, TransportBenchmark.hpp). The direct channel is
// registered here on the browser the adapter created, before navigation: WebMessageReceived and PostWebMessageAsString
// carry the calls of JS to C++ (a call carries an identifier, WebView2 has no reply), ExecuteScript those of C++ to JS.
// It fails on a wrong echo, never on a timing.
//   webview2_transport_benchmark [<report.json>]    (working directory: test/web, needs the Evergreen runtime)
#include "TransportBenchmark.hpp"

#include <algorithm>
#include <cwchar>
#include <memory>
#include <string_view>
#include <utility>

namespace {
namespace wv2 = webfront::frontend::detail::webview2;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using transport_benchmark::Clock;

// Lives as long as the session's browser. A script still running when the window closes completes later, after the
// channel: its callback holds a weak reference and does nothing then.
class DirectChannel : public std::enable_shared_from_this<DirectChannel> {
    ComPtr<ICoreWebView2>            view;
    ComPtr<ICoreWebView2Environment> environment;
    EventRegistrationToken           token{};

    // A buffer shared with the page (ICoreWebView2SharedBuffer): [0, capacity) is written by the page, [capacity,
    // 2 * capacity) by C++. The messages only say which region is ready, so the bytes are never copied through a string.
    ComPtr<ICoreWebView2SharedBuffer> shared;
    BYTE*                             memory   = nullptr;
    std::size_t                       capacity = 0;
    std::wstring                      sharedRunId;
    std::size_t                       sharedSize      = 0;
    int                               sharedRemaining = 0;
    bool                              sharedRunning   = false;
    Clock::time_point                 sharedStart;

    // The C++ to JS run in progress: each script is issued from the completion of the previous one.
    std::wstring      runId;
    std::wstring      expected;
    std::wstring      script;
    int               remaining = 0;
    bool              running   = false;
    Clock::time_point start;

    void post(std::wstring_view id, std::wstring_view text) const {
        std::wstring message(id);
        message += L'\n';
        message += text;
        wv2::check(view->PostWebMessageAsString(message.c_str()), "answering the page");
    }

    void received(ICoreWebView2WebMessageReceivedEventArgs* args) noexcept {
        try {
            LPWSTR raw = nullptr;
            if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw)
                return;
            const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(raw, CoTaskMemFree);
            // "id\nname\ntext"
            const std::wstring_view message(raw);
            const auto              idEnd = message.find(L'\n');
            const auto              nameEnd = idEnd == message.npos ? idEnd : message.find(L'\n', idEnd + 1);
            if (nameEnd == message.npos)
                return;
            const auto id   = message.substr(0, idEnd);
            const auto name = message.substr(idEnd + 1, nameEnd - idEnd - 1);
            const auto text = message.substr(nameEnd + 1);
            if (name == L"wfecho")
                post(id, text);
            else if (name == L"wfrun")
                run(id, text);
            else if (name == L"wfshared")
                createSharedBuffer(id, text);
            else if (name == L"wfsharedecho")
                sharedEcho(id, text);
            else if (name == L"wfsharedrun")
                sharedRun(id, text);
            else if (name == L"wfsharedreply")
                sharedReply(text);
        } catch (...) {
            // The page's call stays pending and its run fails by timeout: the benchmark never reports a wrong figure.
        }
    }

    bool region(const BYTE* begin, std::size_t size) const {
        return std::all_of(begin, begin + size, [](BYTE value) {
            return value == 'x';
        });
    }

    void createSharedBuffer(std::wstring_view id, std::wstring_view total) {
        ComPtr<ICoreWebView2Environment12> factory;
        ComPtr<ICoreWebView2_17>           browser;
        wv2::check(environment.As(&factory), "shared buffers (WebView2 runtime 1.0.2210.55 or later)");
        wv2::check(view.As(&browser), "sharing a buffer with the page");
        const auto bytes = static_cast<UINT64>(std::stoull(std::wstring(total)));
        wv2::check(factory->CreateSharedBuffer(bytes, &shared), "creating the shared buffer");
        wv2::check(shared->get_Buffer(&memory), "mapping the shared buffer");
        capacity = static_cast<std::size_t>(bytes / 2);
        wv2::check(browser->PostSharedBufferToScript(shared.Get(), COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_WRITE, L"{}"), "sharing the buffer with the page");
        post(id, L"ok");
    }

    void sharedEcho(std::wstring_view id, std::wstring_view parameter) {
        const auto size = static_cast<std::size_t>(std::stoull(std::wstring(parameter)));
        if (!memory || size > capacity || !region(memory, size))
            return post(id, L"bad");
        std::fill_n(memory + capacity, size, static_cast<BYTE>('x'));
        post(id, L"ok");
    }

    // C++ to JS: the page reads the region, answers wfsharedreply, and the next call goes out from that answer.
    void sharedRun(std::wstring_view id, std::wstring_view parameters) {
        int size = 0, count = 0;
        if (!memory || swscanf_s(std::wstring(parameters).c_str(), L"%d %d", &size, &count) != 2 || size < 0 || count <= 0 || sharedRunning
            || static_cast<std::size_t>(size) > capacity)
            return;
        sharedRunId     = id;
        sharedSize      = static_cast<std::size_t>(size);
        sharedRemaining = count;
        sharedRunning   = true;
        sharedStart     = Clock::now();
        sharedCall();
    }
    void sharedCall() {
        std::fill_n(memory + capacity, sharedSize, static_cast<BYTE>('x'));
        wv2::check(view->PostWebMessageAsString((L"c\n" + std::to_wstring(sharedSize)).c_str()), "calling the page");
    }
    void sharedReply(std::wstring_view answer) {
        if (!sharedRunning)
            return;
        if (answer != L"ok" || !region(memory, sharedSize)) {
            sharedRunning = false;
            return post(sharedRunId, L"-1");
        }
        if (--sharedRemaining > 0)
            return sharedCall();
        sharedRunning = false;
        post(sharedRunId, std::to_wstring(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - sharedStart).count())));
    }

    void run(std::wstring_view id, std::wstring_view parameters) {
        int size = 0, count = 0;
        if (swscanf_s(std::wstring(parameters).c_str(), L"%d %d", &size, &count) != 2 || size < 0 || count <= 0 || running)
            return;
        const std::wstring payload(static_cast<std::size_t>(size), L'x');
        runId     = id;
        expected  = L"\"" + payload + L"\"";  // ExecuteScript answers the JSON text of the result
        script    = L"benchmark.echo(\"" + payload + L"\")";
        remaining = count;
        running   = true;
        start     = Clock::now();
        evaluateNext();
    }

    void evaluateNext() {
        auto weak    = weak_from_this();
        auto started = view->ExecuteScript(script.c_str(),
                                           Callback<ICoreWebView2ExecuteScriptCompletedHandler>([weak](HRESULT result, LPCWSTR json) -> HRESULT {
                                               if (const auto self = weak.lock())
                                                   self->completed(result, json);
                                               return S_OK;
                                           }).Get());
        if (FAILED(started))
            finish(-1);
    }

    void completed(HRESULT result, LPCWSTR json) noexcept {
        try {
            if (!running)
                return;
            if (FAILED(result) || !json || expected != json)
                finish(-1);
            else if (--remaining == 0)
                finish(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()));
            else
                evaluateNext();
        } catch (...) {
            running = false;
        }
    }

    void finish(double nanoseconds) {
        running = false;
        post(runId, std::to_wstring(nanoseconds));
    }

public:
    DirectChannel(ComPtr<ICoreWebView2> browser, ComPtr<ICoreWebView2Environment> factory) : view(std::move(browser)), environment(std::move(factory)) { }
    DirectChannel(const DirectChannel&)            = delete;
    DirectChannel& operator=(const DirectChannel&) = delete;
    ~DirectChannel() {
        // The browser may already be closed; there is then nothing to unregister.
        if (view && token.value)
            view->remove_WebMessageReceived(token);
    }
    void attach() {
        auto weak = weak_from_this();
        wv2::check(view->add_WebMessageReceived(
                       Callback<ICoreWebView2WebMessageReceivedEventHandler>([weak](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                           if (const auto self = weak.lock())
                               self->received(args);
                           return S_OK;
                       }).Get(),
                       &token),
                   "registering the direct message channel");
    }
};

// The WebView2 window of the adapter, with the direct channel registered before navigation.
struct BenchmarkFrontend : webfront::frontend::WebView2Frontend {
    static void open(std::string_view port, std::string_view page) {
        open("127.0.0.1", port, page, webfront::ContentPolicy{});
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const webfront::ContentPolicy& policy) {
        webfront::frontend::WebView2Options options;
        options.contentPolicy = policy;
        // The channel lives in the observer, so in the session, and goes with it.
        wv2::open(address, port, page, options, [](wv2::Session& session) {
            session.viewReady = [&session, channel = std::shared_ptr<DirectChannel>()]() mutable {
                channel = std::make_shared<DirectChannel>(session.view, session.environment);
                channel->attach();
            };
        });
    }
};
}  // namespace

int main(int argc, char** argv) {
    return transport_benchmark::run<BenchmarkFrontend>(argc, argv, "usage: webview2_transport_benchmark [<report.json>]");
}
