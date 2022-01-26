/// @date 26/01/2022 11:38:00
/// @author Ambroise Leclerc
/// @brief C++2x networking TS from std::experimental::net
#pragma once
#include "BasicNetworking.hpp"

#include <experimental/net>

namespace webfront {
namespace networking {

class TCPNetworkingTS : public BasicNetworking<std::experimental::net::const_buffer, std::experimental::net::mutable_buffer> {
public:
    using Acceptor = std::experimental::net::ip::tcp::acceptor;
    using Endpoint = std::experimental::net::ip::tcp::endpoint;
    using IoContext = std::experimental::net::io_context;
    using Resolver = std::experimental::net::ip::tcp::resolver;
    using Socket = std::experimental::net::ip::tcp::socket;
    using super::ConstBuffer;
    using super::MutableBuffer;

    template<typename... Args>
    static auto AsyncWrite(Args&&... args) -> decltype(std::experimental::net::async_write(std::forward<Args>(args)...)) {
        return std::experimental::net::async_write(std::forward<Args>(args)...);
    }

    template<typename... Args>
    static auto Write(Args&&... args) -> decltype(std::experimental::net::write(std::forward<Args>(args)...)) {
        return std::experimental::net::write(std::forward<Args>(args)...);
    }

    struct Error {
        static inline auto OperationAborted = std::experimental::net::error::operation_aborted;
    };
};

} // namespace networking
} // namespace webfront
