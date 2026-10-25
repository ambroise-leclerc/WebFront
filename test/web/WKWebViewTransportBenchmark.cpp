// ADR-017: one WKWebView, the HTTP bridge and public Cocoa message/evaluation APIs.
// Kept in C++ like the adapter: Objective-C runtime calls and Clang Blocks, without ARC.
#include "TransportBenchmark.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

#include <Block.h>

namespace {
namespace wk = webfront::frontend::detail::wkwebview;
using transport_benchmark::Clock;
using Reply = void (^)(id, id);

class DirectChannel : public std::enable_shared_from_this<DirectChannel> {
    wk::Session&      session;
    wk::Object        manager;
    wk::Object        handler;
    Reply             reply = nullptr;
    wk::Object        asyncBody{wk::text("return benchmark.echo(payload);")};
    wk::Object        binaryBody{wk::text("return btoa(benchmark.echo(atob(payload)));")};
    id                script = nullptr, arguments = nullptr;
    id                binaryPayload = nullptr;
    std::string       expected;
    int               remaining = 0;
    bool              async     = false;
    bool              bytes     = false;
    Clock::time_point start;

    static void reject(Reply answer, std::string_view message) {
        wk::Object error(wk::text(message));
        answer(nullptr, error.get());
    }

    static void received(id self, SEL, id, id message, Reply answer) noexcept {
        try {
            void* pointer = nullptr;
            object_getInstanceVariable(self, "webfrontSession", &pointer);
            if (auto* channel = static_cast<DirectChannel*>(pointer))
                channel->receive(message, answer);
            else
                reject(answer, "channel closed");
        } catch (const std::exception& error) {
            reject(answer, error.what());
        }
    }

    void receive(id message, Reply answer) {
        const auto frame  = wk::send(message, "frameInfo");
        const auto origin = wk::send(frame, "securityOrigin");
        const auto scheme = wk::utf8(wk::send(origin, "protocol"));
        const auto host   = wk::utf8(wk::send(origin, "host"));
        const auto port   = wk::send<long>(origin, "port");
        const auto source = scheme + "://" + host + ":" + std::to_string(port);
        if (!wk::send<BOOL>(frame, "isMainFrame") || !session.contentPolicy.allowsBridge(source))
            return reject(answer, "frame or origin denied");
        const auto name = wk::utf8(wk::send(message, "name"));
        const auto body = wk::send(message, "body");
        if (name == "wfecho") {
            answer(body, nullptr);
        } else if (name == "wfbytes") {
            wk::Object data(wk::send(wk::send(wk::type("NSData"), "alloc"), "initWithBase64EncodedString:options:", body, 0UL));
            if (!data.get())
                return reject(answer, "invalid base64");
            answer(wk::send(data.get(), "base64EncodedStringWithOptions:", 0UL), nullptr);
        } else if (name == "wfprobe") {
            const auto command = wk::utf8(body);
            if (command == "error")
                reject(answer, "native probe error");
            else if (command == "schemes") {
                wk::Object http(wk::text("http")), https(wk::text("https")), custom(wk::text("webfront-benchmark"));
                const auto view      = wk::type("WKWebView");
                const bool supported = wk::send<BOOL>(view, "handlesURLScheme:", http.get()) && wk::send<BOOL>(view, "handlesURLScheme:", https.get())
                                       && !wk::send<BOOL>(view, "handlesURLScheme:", custom.get());
                answer(wk::send(wk::type("NSNumber"), "numberWithBool:", static_cast<BOOL>(supported)), nullptr);
            }
            // Deliberately leave "unanswered" without copying/calling its reply: WebKit must reject it.
            else if (command != "unanswered")
                reject(answer, "unknown probe");
        } else {
            int size = 0, count = 0;
            if (std::sscanf(wk::utf8(body).c_str(), "%d %d", &size, &count) != 2 || size < 0 || size > 32768 || count <= 0 || reply)
                return reject(answer, "invalid or overlapping run");
            expected = std::string(static_cast<std::size_t>(size), 'x');
            wk::Object code(wk::text("benchmark.echo(\"" + expected + "\")"));
            wk::Object payload(wk::text(expected));
            wk::Object key(wk::text("payload"));
            const auto dictionary = wk::send(wk::type("NSDictionary"), "dictionaryWithObject:forKey:", payload.get(), key.get());
            script                = wk::send(code.get(), "retain");
            arguments             = wk::send(dictionary, "retain");
            binaryPayload         = wk::send(wk::send(wk::type("NSData"), "dataWithBytes:length:", expected.data(), expected.size()), "retain");
            reply                 = Block_copy(answer);  // The handler returns before a C++ -> JS batch finishes.
            remaining             = count;
            bytes                 = name == "wfbytesrun";
            async                 = name == "wfasync" || bytes;
            start                 = Clock::now();
            evaluateNext();
        }
    }

    void evaluateNext() {
        const auto weak            = weak_from_this();
        void (^completion)(id, id) = ^(id value, id error) {
            if (const auto channel = weak.lock())
                channel->completed(value, error);
        };
        if (async) {
            auto parameters = arguments;
            if (bytes) {
                wk::Object key(wk::text("payload"));
                parameters = wk::send(wk::type("NSDictionary"),
                                      "dictionaryWithObject:forKey:",
                                      wk::send(binaryPayload, "base64EncodedStringWithOptions:", 0UL),
                                      key.get());
            }
            wk::send<void>(session.view,
                           "callAsyncJavaScript:arguments:inFrame:inContentWorld:completionHandler:",
                           bytes ? binaryBody.get() : asyncBody.get(),
                           parameters,
                           static_cast<id>(nullptr),
                           wk::send(wk::type("WKContentWorld"), "pageWorld"),
                           completion);
        } else
            wk::send<void>(session.view, "evaluateJavaScript:completionHandler:", script, completion);
    }

    void completed(id value, id error) noexcept {
        try {
            if (!reply)
                return;
            bool echoed = !error && value && wk::send<BOOL>(value, "isKindOfClass:", wk::type("NSString"));
            if (echoed && bytes) {
                wk::Object decoded(wk::send(wk::send(wk::type("NSData"), "alloc"), "initWithBase64EncodedString:options:", value, 0UL));
                echoed = decoded.get() && wk::send<std::size_t>(decoded.get(), "length") == expected.size()
                         && (expected.empty() || std::memcmp(wk::send<const void*>(decoded.get(), "bytes"), expected.data(), expected.size()) == 0);
            } else if (echoed)
                echoed = wk::utf8(value) == expected;
            if (!echoed)
                finish(-1);
            else if (--remaining == 0)
                finish(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()));
            else
                evaluateNext();
        } catch (...) {
            if (reply)
                finish(-1);
        }
    }

    void finish(double nanoseconds) {
        const auto answer = std::exchange(reply, nullptr);
        answer(wk::send(wk::type("NSNumber"), "numberWithDouble:", nanoseconds), nullptr);
        Block_release(answer);
        wk::send<void>(std::exchange(script, nullptr), "release");
        wk::send<void>(std::exchange(arguments, nullptr), "release");
        wk::send<void>(std::exchange(binaryPayload, nullptr), "release");
    }

public:
    explicit DirectChannel(wk::Session& live)
        : session(live),
          manager(wk::send(wk::send(wk::send(live.view, "configuration"), "userContentController"), "retain")),
          handler(wk::send(
              reinterpret_cast<id>(wk::registerClass("WebFrontWKBenchmarkHandler_v1",
                                                     reinterpret_cast<Class>(wk::type("NSObject")),
                                                     [](Class cls) {
                                                         class_addProtocol(cls, objc_getProtocol("WKScriptMessageHandlerWithReply"));
                                                         wk::addMethod(cls, "userContentController:didReceiveScriptMessage:replyHandler:", received, "v@:@@@?");
                                                     })),
              "new")) {
        object_setInstanceVariable(handler.get(), "webfrontSession", this);
    }
    DirectChannel(const DirectChannel&)            = delete;
    DirectChannel& operator=(const DirectChannel&) = delete;
    void           attach() {
        for (const auto name : {"wfecho", "wfrun", "wfasync", "wfbytes", "wfbytesrun", "wfprobe"}) {
            wk::Object label(wk::text(name));
            wk::send<void>(manager.get(),
                           "addScriptMessageHandlerWithReply:contentWorld:name:",
                           handler.get(),
                           wk::send(wk::type("WKContentWorld"), "pageWorld"),
                           label.get());
        }
    }
    ~DirectChannel() {
        object_setInstanceVariable(handler.get(), "webfrontSession", nullptr);
        for (const auto name : {"wfecho", "wfrun", "wfasync", "wfbytes", "wfbytesrun", "wfprobe"}) {
            wk::Object label(wk::text(name));
            wk::send<void>(manager.get(), "removeScriptMessageHandlerForName:contentWorld:", label.get(), wk::send(wk::type("WKContentWorld"), "pageWorld"));
        }
        if (reply) {
            reject(reply, "channel closed");
            Block_release(reply);
        }
        wk::send<void>(script, "release");
        wk::send<void>(arguments, "release");
        wk::send<void>(binaryPayload, "release");
    }
};

struct BenchmarkFrontend : webfront::frontend::WKWebViewFrontend {
    static void open(std::string_view port, std::string_view page) {
        open("127.0.0.1", port, page, {});
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const webfront::ContentPolicy& policy) {
        wk::initialize();
        wk::Object  pool(wk::send(wk::type("NSAutoreleasePool"), "new"));
        wk::Session session(webfront::frontend::detail::applicationUrl(address, port, page), {.contentPolicy = policy});
        session.create();
        const auto channel = std::make_shared<DirectChannel>(session);
        channel->attach();
        session.start();
        session.run();
    }
};
}  // namespace

int main(int argc, char** argv) {
#ifdef WEBFRONT_TRANSPORT_CONTRACTS_ONLY
    const bool contracts = true;
#else
    const bool contracts = argc == 2 && std::string_view(argv[1]) == "--contracts";
#endif
    return transport_benchmark::run<BenchmarkFrontend>(contracts ? 1 : argc,
                                                       argv,
                                                       "usage: wkwebview_transport_benchmark [<report.json>|--contracts]",
                                                       contracts ? "wkwebview-transport-contracts.html" : "transport-benchmark.html");
}
