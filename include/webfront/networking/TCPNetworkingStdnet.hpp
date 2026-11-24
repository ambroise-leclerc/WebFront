// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-13
//
// WebFront — networking
// Role: WebFront networking provider backed by stdnet: operations are senders awaited by coroutines.
// Thread: bridge-io | Alloc: unrestricted
// LLM: default provider (TCPStdnet); all socket objects close on the io_context thread

// WebFront networking provider backed by stdnet (prospective C++29 <network>).
//
// Operations are the senders of stdnet, awaited by coroutines.
// Usage:  using NetProvider = webfront::networking::TCPStdnet;
//         BasicWF<NetProvider, MyFS> webFront(port, docRoot);

#pragma once

#include "BasicNetworking.hpp"
#include "SenderHeaders.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <system_error>

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
    static auto Scheduler(auto& ioObject) {
        return ioObject.context().get_scheduler();
    }

    /// Completes with the number of bytes read, zero at the end of the stream.
    static auto Receive(Socket& socket, MutableBuffer buffer) {
        return ::stdnet::async_receive(socket, buffer);
    }
    static auto Send(Socket& socket, const auto& buffers) {
        return ::stdnet::async_write(socket, buffers);
    }
    /// Completes with an accepted socket whose Nagle algorithm is disabled. The option is a latency optimisation: macOS refuses it
    /// (EINVAL) on a connection that the peer already reset, which is not an accept failure. The socket is delivered anyway and
    /// its first read or write reports the closed connection (#628).
    static auto Accept(Acceptor& acceptor) {
        return ::stdnet::ex::then(::stdnet::async_accept(acceptor), [](Socket socket) {
            try {
                socket.set_option(::stdnet::ip::tcp_no_delay(true));
            } catch (const std::system_error&) { }
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
