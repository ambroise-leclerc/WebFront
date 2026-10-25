// ADR-017 (#363): measures the HTML bridge (WebSocket to the embedded server) against WebKitGTK's own message channel,
// in the same window and with the same echo calls (transport-benchmark.html, TransportBenchmark.hpp). The direct channel is
// registered here, through the C ABI the adapter already loaded: script message handlers with a reply for JS to C++, and
// webkit_web_view_evaluate_javascript for C++ to JS. It fails on a wrong echo, never on a timing.
//   webkitgtk_transport_benchmark [<report.json>]    (working directory: test/web, needs a display)
#include "TransportBenchmark.hpp"

#include <cstdio>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <dlfcn.h>

namespace {
namespace gtk = webfront::frontend::detail::webkitgtk;
using Object  = gtk::Api::Object;
using transport_benchmark::Clock;

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
    void (*unref)(Object)                                       = symbol<decltype(unref)>("g_object_unref");

    // One table for the process: an evaluation that completes after its channel needs it to release its result.
    static const DirectApi& shared() {
        static const DirectApi table;
        return table;
    }
};

// Lives inside the session's scope and disconnects its handlers before the view goes away. An evaluation still pending
// when the window closes completes later, after the channel: it holds a weak reference and does nothing then.
class DirectChannel : public std::enable_shared_from_this<DirectChannel> {
    gtk::Session&              session;
    const DirectApi&           api     = DirectApi::shared();
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

    // Handlers run in the main loop and must not throw: an allocation failure leaves the message unhandled, and the page's
    // call rejects.
    static int onEcho(Object, Object value, Object message, DirectChannel* self) noexcept {
        try {
            auto& api    = self->api;
            auto  answer = api.newString(api.valueContext(value), self->text(value).c_str());
            api.replyValue(message, answer);
            self->session.a.unref(answer);
            return 1;
        } catch (...) {
            return 0;
        }
    }
    static int onRun(Object, Object value, Object message, DirectChannel* self) noexcept {
        try {
            auto& api  = self->api;
            int   size = 0, count = 0;
            if (std::sscanf(self->text(value).c_str(), "%d %d", &size, &count) != 2 || size < 0 || count <= 0 || self->reply)
                return 0;  // Unhandled: the page receives no duration and its run fails.
            self->payload   = std::string(static_cast<std::size_t>(size), 'x');
            self->script    = "benchmark.echo(\"" + self->payload + "\")";
            self->reply     = api.replyRef(message);
            self->context   = api.ref(api.valueContext(value));
            self->remaining = count;
            self->start     = Clock::now();
            self->evaluateNext();
            return 1;
        } catch (...) {
            return 0;
        }
    }
    void evaluateNext() {
        auto* pending = new std::weak_ptr<DirectChannel>(weak_from_this());
        session.a.evaluate(session.view,
                           script.c_str(),
                           static_cast<long>(script.size()),
                           nullptr,
                           nullptr,
                           nullptr,
                           reinterpret_cast<gtk::Api::Callback>(onEvaluated),
                           pending);
    }
    // Always called once per evaluation, with an error when the view is destroyed first.
    static void onEvaluated(Object view, Object result, std::weak_ptr<DirectChannel>* pending) noexcept {
        const std::unique_ptr<std::weak_ptr<DirectChannel>> owned(pending);
        const auto&                                          api   = DirectApi::shared();
        gtk::Api::Error*                                     error = nullptr;
        const auto                                           value = api.evaluateFinish(view, result, &error);
        if (const auto self = owned->lock())
            return self->completed(value, error);
        // The window closed first: the channel and its session are gone, the result is only released.
        if (value)
            api.unref(value);
        if (error)
            api.errorFree(error);
    }
    void completed(Object value, gtk::Api::Error* error) noexcept {
        bool echoed = false;
        try {
            echoed = value && text(value) == payload;
        } catch (...) {
        }
        if (value)
            session.a.unref(value);
        if (error)
            api.errorFree(error);
        if (!echoed)
            finish(-1);
        else if (--remaining == 0)
            finish(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()));
        else
            evaluateNext();
    }
    void finish(double nanoseconds) {
        auto answer = api.newNumber(context, nanoseconds);
        api.replyValue(reply, answer);
        session.a.unref(answer);
        api.replyUnref(std::exchange(reply, nullptr));
        session.a.unref(std::exchange(context, nullptr));
    }

public:
    explicit DirectChannel(gtk::Session& live) : session(live) { }
    void attach() {
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
        const auto channel = std::make_shared<DirectChannel>(session);
        channel->attach();
        session.run(webfront::frontend::detail::applicationUrl(address, port, page));
    }
};
}  // namespace

int main(int argc, char** argv) {
#ifdef WEBFRONT_TRANSPORT_CONTRACTS_ONLY
    (void)argc;
    return transport_benchmark::run<BenchmarkFrontend>(1, argv, "", "transport-contracts.html");
#else
    return transport_benchmark::run<BenchmarkFrontend>(argc, argv, "usage: webkitgtk_transport_benchmark [<report.json>]");
#endif
}
