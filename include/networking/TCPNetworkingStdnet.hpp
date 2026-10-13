/// @brief WebFront networking provider backed by stdnet (prospective C++29 <network>).
///
/// Drop-in replacement for networking/TCPNetworkingTS.hpp: it exposes the callback-style
/// surface WebFront uses today (async_read_some / async_accept / AsyncWrite with completion
/// handlers) on top of stdnet's senders, so WebFront can move off the Networking TS now and
/// migrate to senders/coroutines incrementally.
///
/// Usage:  using NetProvider = webfront::networking::TCPStdnet;
///         BasicWF<NetProvider, MyFS> webFront(port, docRoot);
#pragma once

#include "BasicNetworking.hpp"

#include <network>

#include <cstddef>
#include <exception>
#include <vector>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace webfront::networking {

namespace stdnet_bridge {

namespace ex = ::stdnet::ex;

// Owns the operation state of a submitted sender; deletes itself after invoking the handler.
template <class Sndr, class Handler>
struct submit_holder {
    struct receiver {
        using receiver_concept = ex::receiver_t;
        submit_holder* h;
        template <class... Vs>
        void set_value(Vs&&... vs) && noexcept {
            h->finish(std::error_code{}, std::forward<Vs>(vs)...);
        }
        void set_error(std::error_code ec) && noexcept { h->finish(ec); }
        void set_error(std::exception_ptr) && noexcept { h->finish(std::make_error_code(std::errc::io_error)); }
        void set_stopped() && noexcept { h->finish(std::make_error_code(std::errc::operation_canceled)); }
        ex::env<> get_env() const noexcept { return {}; }
    };

    Handler handler;
    ex::connect_result_t<Sndr, receiver> op;

    template <class S, class F>
    submit_holder(S&& s, F&& f) : handler(std::forward<F>(f)), op(ex::connect(std::forward<S>(s), receiver{this})) {}

    template <class... Vs>
    void finish(std::error_code ec, Vs&&... vs) noexcept {
        if constexpr (sizeof...(Vs) > 0) {
            handler(ec, std::forward<Vs>(vs)...);
        } else if constexpr (std::is_invocable_v<Handler&, std::error_code, std::size_t>) {
            handler(ec, std::size_t{0}); // failure: values default to "nothing transferred"
        } else {
            handler(ec);
        }
        delete this;
    }
};

// Starts a sender and invokes handler(error_code, values...) on completion (Networking TS style).
template <class Sndr, class Handler>
void submit(Sndr&& sndr, Handler&& handler) {
    using holder = submit_holder<std::decay_t<Sndr>, std::decay_t<Handler>>;
    auto* h = new holder(std::forward<Sndr>(sndr), std::forward<Handler>(handler));
    ex::start(h->op);
}

// End of stream, reported as an error to keep the Networking TS behaviour WebFront relies on.
inline std::error_code eof() noexcept { return std::make_error_code(std::errc::connection_reset); }

} // namespace stdnet_bridge

class TCPStdnet : public BasicNetworking<::stdnet::const_buffer, ::stdnet::mutable_buffer> {
public:
    using IoContext = ::stdnet::io_context;
    using Endpoint = ::stdnet::ip::tcp::endpoint;
    using super::ConstBuffer;
    using super::MutableBuffer;

    class Socket : public ::stdnet::ip::tcp::socket {
    public:
        using base = ::stdnet::ip::tcp::socket;
        explicit Socket(IoContext& ctx) noexcept : base(ctx) {}
        Socket(base&& s) noexcept : base(std::move(s)) {}
        Socket(Socket&&) noexcept = default;
        Socket& operator=(Socket&&) noexcept = default;

        template <::stdnet::mutable_buffer_sequence Buffers, class Handler>
        void async_read_some(const Buffers& buffers, Handler&& handler) {
            stdnet_bridge::submit(::stdnet::async_receive(*this, buffers),
                                  [h = std::forward<Handler>(handler), size = ::stdnet::buffer_size(buffers)](
                                      std::error_code ec, std::size_t n) mutable {
                                      if (!ec && n == 0 && size > 0) ec = stdnet_bridge::eof();
                                      h(ec, n);
                                  });
        }
    };

    class Acceptor : public ::stdnet::ip::tcp::acceptor {
    public:
        explicit Acceptor(IoContext& ctx) noexcept : ::stdnet::ip::tcp::acceptor(ctx) {}

        template <class Handler>
        void async_accept(Handler&& handler) {
            struct adapter {
                std::decay_t<Handler> h;
                IoContext* ctx;
                void operator()(std::error_code ec, ::stdnet::ip::tcp::socket s) { h(ec, Socket(std::move(s))); }
                void operator()(std::error_code ec, std::size_t) { h(ec, Socket(*ctx)); }
            };
            stdnet_bridge::submit(::stdnet::async_accept(static_cast<::stdnet::ip::tcp::acceptor&>(*this)),
                                  adapter{std::forward<Handler>(handler), &context()});
        }
    };

    class Resolver {
    public:
        explicit Resolver(IoContext&) noexcept {}
        template <class Host, class Service>
        ::stdnet::ip::resolve_results<::stdnet::ip::tcp> resolve(const Host& host, const Service& service) {
            return ::stdnet::ip::resolve<::stdnet::ip::tcp>(std::string(host), std::string(service));
        }
    };

    template <class Buffers, class Handler>
    static void AsyncWrite(Socket& socket, Buffers&& buffers, Handler&& handler) {
        stdnet_bridge::submit(::stdnet::async_write(socket, std::forward<Buffers>(buffers)),
                              std::forward<Handler>(handler));
    }

    template <class Buffers>
    static std::size_t Write(Socket& socket, const Buffers& buffers) {
        std::size_t total = 0, wanted = ::stdnet::buffer_size(buffers);
        std::vector<::stdnet::const_buffer> remaining;
        ::stdnet::for_each_buffer<::stdnet::const_buffer>(buffers, [&](::stdnet::const_buffer b) { remaining.push_back(b); });
        while (total < wanted) {
            std::size_t skip = total;
            std::vector<::stdnet::const_buffer> view;
            for (auto b : remaining) {
                if (skip >= b.size()) {
                    skip -= b.size();
                    continue;
                }
                view.push_back(b + skip);
                skip = 0;
            }
            total += socket.send(view);
        }
        return total;
    }

    struct Error {
        static inline const std::error_code OperationAborted = std::make_error_code(std::errc::operation_canceled);
    };
};

} // namespace webfront::networking
