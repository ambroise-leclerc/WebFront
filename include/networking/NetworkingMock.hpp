/// @date 26/01/2022 11:38:14
/// @author Ambroise Leclerc
/// @brief a Networking mock implementation for testing purposes
#pragma once
#include "BasicNetworking.hpp"

#include  <system_error>

namespace webfront::networking {

class SocketMock {
public:
    void async_read_some(auto /*Buffer*/, auto /*completionFunction*/) {

    }

    void close() {
        
    }
};

class NetworkingMock : public BasicNetworking<> {
public:
    using super::ConstBuffer;
    using super::MutableBuffer;
    using Socket = SocketMock;

    template<typename... Args>
    static auto AsyncWrite(Args&&... /*args*/) -> void {
        return;
    }

    struct Error {
        static inline const auto OperationAborted = std::make_error_code(std::errc::operation_canceled);
    };
};

} // namespace webfront::networking
