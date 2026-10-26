/// @brief WebFront networking provider backed by stdnet (prospective C++29 <network>).
///
/// Operations are the senders of stdnet, awaited by coroutines.
/// Usage:  using NetProvider = webfront::networking::TCPStdnet;
///         BasicWF<NetProvider, MyFS> webFront(port, docRoot);
#pragma once

#include "BasicNetworking.hpp"
#include "SenderHeaders.hpp"

#include <chrono>
#include <string>
#include <string_view>

namespace webfront::networking {

class TCPStdnet : public BasicNetworking<::stdnet::const_buffer, ::stdnet::mutable_buffer> {
public:
    using Acceptor  = ::stdnet::ip::tcp::acceptor;
    using Endpoint  = ::stdnet::ip::tcp::endpoint;
    using IoContext = ::stdnet::io_context;
    using Socket    = ::stdnet::ip::tcp::socket;
    using super::ConstBuffer;
    using super::MutableBuffer;

    static Endpoint Resolve(std::string_view address, std::string_view port) {
        return *::stdnet::ip::resolve<::stdnet::ip::tcp>(std::string(address), std::string(port)).begin();
    }

    /// The scheduler running the operations of a socket or an acceptor.
    static auto Scheduler(auto& ioObject) { return ioObject.context().get_scheduler(); }

    /// Completes with the number of bytes read, zero at the end of the stream.
    static auto Receive(Socket& socket, MutableBuffer buffer) { return ::stdnet::async_receive(socket, buffer); }
    static auto Send(Socket& socket, const auto& buffers) { return ::stdnet::async_write(socket, buffers); }
    /// Disable Nagle before handing the connection to HTTP, retaining the option after WebSocket upgrade.
    static auto Accept(Acceptor& acceptor) {
        return ::stdnet::ex::then(::stdnet::async_accept(acceptor), [](Socket socket) {
            socket.set_option(::stdnet::ip::tcp_no_delay(true));
            return socket;
        });
    }
    /// Completes on the context of a socket or an acceptor once the duration has elapsed, or stops first on a stop request.
    template <typename Rep, typename Period>
    static auto ResumeAfter(auto& ioObject, std::chrono::duration<Rep, Period> duration) {
        return ::stdnet::async_resume_after(Scheduler(ioObject), duration);
    }
};

}  // namespace webfront::networking
