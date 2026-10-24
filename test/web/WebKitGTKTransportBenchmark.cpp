// ADR-017 (#363): measures the HTML bridge (WebSocket to the embedded server) against WebKitGTK's own message channel,
// in the same window and with the same echo calls (transport-benchmark.html). The direct channel is registered here,
// through the C ABI the adapter already loaded: script message handlers with a reply for JS to C++, and
// webkit_web_view_evaluate_javascript for C++ to JS. It fails on a wrong echo, never on a timing.
//   webkitgtk_transport_benchmark [<report.json>]    (working directory: test/web, needs a display)
#include <webfront/system/NativeFS.hpp>
#include <webfront/WebFront.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dlfcn.h>

namespace {
namespace gtk = webfront::frontend::detail::webkitgtk;
using Object  = gtk::Api::Object;
using Clock   = std::chrono::steady_clock;

// The entry points of the direct channel, resolved from the WebKitGTK module the adapter keeps loaded (WebKitGTK 2.40+).
class DirectApi {
    void* module = dlopen("libwebkitgtk-6.0.so.4", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);

    template <typename Function>
    Function symbol(const char* name) {
        if (!module)
            throw std::runtime_error("WebKitGTK is not loaded");
        const auto result = reinterpret_cast<Function>(dlsym(module, name));
        if (!result)
            throw std::runtime_error(std::string("WebKitGTK: missing runtime symbol ") + name);
        return result;
    }

public:
    Object (*contentManager)(Object)      = symbol<decltype(contentManager)>("webkit_web_view_get_user_content_manager");
    int (*registerWithReply)(Object,
                             const char*,
                             const char*) = symbol<decltype(registerWithReply)>("webkit_user_content_manager_register_script_message_handler_with_reply");
    void (*replyValue)(Object, Object)    = symbol<decltype(replyValue)>("webkit_script_message_reply_return_value");
    Object (*replyRef)(Object)            = symbol<decltype(replyRef)>("webkit_script_message_reply_ref");
    void (*replyUnref)(Object)            = symbol<decltype(replyUnref)>("webkit_script_message_reply_unref");
    Object (*evaluateFinish)(Object, Object, gtk::Api::Error**) = symbol<decltype(evaluateFinish)>("webkit_web_view_evaluate_javascript_finish");
    char* (*toString)(Object)                                   = symbol<decltype(toString)>("jsc_value_to_string");
    Object (*valueContext)(Object)                              = symbol<decltype(valueContext)>("jsc_value_get_context");
    Object (*newString)(Object, const char*)                    = symbol<decltype(newString)>("jsc_value_new_string");
    Object (*newNumber)(Object, double)                         = symbol<decltype(newNumber)>("jsc_value_new_number");
    Object (*ref)(Object)                                       = symbol<decltype(ref)>("g_object_ref");
    void (*free)(void*)                                         = symbol<decltype(free)>("g_free");
    void (*errorFree)(gtk::Api::Error*)                         = symbol<decltype(errorFree)>("g_error_free");
};

// Lives inside the session's scope and disconnects its handlers before the view goes away.
class DirectChannel {
    gtk::Session&              session;
    DirectApi                  api;
    Object                     manager = api.contentManager(session.view);
    std::vector<unsigned long> handlers;

    // The C++ to JS run in progress: each evaluation is issued from the completion of the previous one.
    Object            reply   = nullptr;
    Object            context = nullptr;
    std::string       payload;
    std::string       script;
    int               remaining = 0;
    Clock::time_point start;

    std::string text(Object value) const {
        auto*       raw = api.toString(value);
        std::string result(raw ? raw : "");
        api.free(raw);
        return result;
    }
    void listen(const char* name, auto callback) {
        if (!api.registerWithReply(manager, name, nullptr))
            throw std::runtime_error(std::string("WebKitGTK refused the script message handler ") + name);
        const auto signal = std::string("script-message-with-reply-received::") + name;
        handlers.push_back(session.a.connect(manager, signal.c_str(), reinterpret_cast<gtk::Api::Callback>(callback), this, nullptr, 0));
    }

    static int onEcho(Object, Object value, Object message, DirectChannel* self) noexcept {
        auto& api    = self->api;
        auto  answer = api.newString(api.valueContext(value), self->text(value).c_str());
        api.replyValue(message, answer);
        self->session.a.unref(answer);
        return 1;
    }
    static int onRun(Object, Object value, Object message, DirectChannel* self) noexcept {
        auto& api  = self->api;
        int   size = 0, count = 0;
        if (std::sscanf(self->text(value).c_str(), "%d %d", &size, &count) != 2 || size < 0 || count <= 0 || self->reply)
            return 0;  // Unhandled: the page receives no duration and its run fails.
        self->reply     = api.replyRef(message);
        self->context   = api.ref(api.valueContext(value));
        self->payload   = std::string(static_cast<std::size_t>(size), 'x');
        self->script    = "benchmark.echo(\"" + self->payload + "\")";
        self->remaining = count;
        self->start     = Clock::now();
        self->evaluateNext();
        return 1;
    }
    void evaluateNext() {
        session.a.evaluate(session.view,
                           script.c_str(),
                           static_cast<long>(script.size()),
                           nullptr,
                           nullptr,
                           nullptr,
                           reinterpret_cast<gtk::Api::Callback>(onEvaluated),
                           this);
    }
    static void onEvaluated(Object view, Object result, DirectChannel* self) noexcept {
        gtk::Api::Error* error  = nullptr;
        auto             value  = self->api.evaluateFinish(view, result, &error);
        const bool       echoed = value && self->text(value) == self->payload;
        if (value)
            self->session.a.unref(value);
        if (error)
            self->api.errorFree(error);
        if (!echoed)
            self->finish(-1);
        else if (--self->remaining == 0)
            self->finish(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - self->start).count()));
        else
            self->evaluateNext();
    }
    void finish(double nanoseconds) {
        auto answer = api.newNumber(context, nanoseconds);
        api.replyValue(reply, answer);
        session.a.unref(answer);
        api.replyUnref(std::exchange(reply, nullptr));
        session.a.unref(std::exchange(context, nullptr));
    }

public:
    explicit DirectChannel(gtk::Session& live) : session(live) {
        listen("wfecho", onEcho);
        listen("wfrun", onRun);
    }
    DirectChannel(const DirectChannel&)            = delete;
    DirectChannel& operator=(const DirectChannel&) = delete;
    ~DirectChannel() {
        for (auto handler : handlers)
            session.a.disconnect(manager, handler);
        if (reply)
            api.replyUnref(reply);
        if (context)
            session.a.unref(context);
    }
};

// The WebKitGTK window of the adapter, with the direct channel registered before navigation.
struct BenchmarkFrontend : webfront::frontend::WebKitGTKFrontend {
    static void open(std::string_view port, std::string_view page) {
        open("127.0.0.1", port, page, {});
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const webfront::ContentPolicy& policy) {
        gtk::initialize();
        gtk::Session session(webfront::detail::BoundContentPolicy{webfront::detail::applicationOrigin(address, port), policy});
        session.create();
        DirectChannel channel(session);
        session.run(webfront::frontend::detail::applicationUrl(address, port, page));
    }
};

using TestFS = webfront::fs::Multi<webfront::fs::IndexFS, webfront::fs::NativeDebugFS>;
using App    = webfront::BasicWFWithFrontend<webfront::NetProvider, TestFS, BenchmarkFrontend>;

// Each call awaits the previous result, as the direct run does; the page receives the elapsed nanoseconds, or -1.
webfront::Task cppToJs(App::UI ui, int size, int count) {
    const std::string payload(static_cast<std::size_t>(size), 'x');
    double            elapsed = -1;
    try {
        const auto start = Clock::now();
        for (int i = 0; i < count; ++i)
            if (co_await ui.jsFunctionSender<std::string>("benchmark.echo")(payload) != payload)
                throw std::runtime_error("wrong echo");
        elapsed = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    } catch (const std::exception& error) {
        std::cerr << "C++ to JS bridge echo failed: " << error.what() << '\n';
    }
    ui.jsFunction("benchmark.cppPhaseDone")(elapsed);
}

struct Report {
    bool        received = false;
    bool        passed   = false;
    std::string json;
    std::string text;
};
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 2)
            throw std::runtime_error("usage: webkitgtk_transport_benchmark [<report.json>]");
        Report report;
        App    app("9002", std::filesystem::current_path());
        app.cppFunction<std::string, std::string>("echo", [](const std::string& value) {
            return value;
        });
        app.cppFunction("cppToJs", [&app](App::UI ui, int size, int count) {
            app.spawn(cppToJs(ui, size, count));
        });
        app.cppFunction<void, bool, std::string, std::string>("report", [&report](bool passed, const std::string& json, const std::string& text) {
            report = {true, passed, json, text};
        });
        // show() returns once the window is closed and the server joined: the report is no longer written to.
        app.show("transport-benchmark.html");
        if (!report.received)
            throw std::runtime_error("the benchmark page closed without a report");
        std::cout << report.text << '\n';
        if (!report.passed)
            return 1;
        if (argc == 2) {
            std::ofstream output(argv[1], std::ios::binary);
            output << report.json << '\n';
            if (!output)
                throw std::runtime_error(std::string("cannot write ") + argv[1]);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
