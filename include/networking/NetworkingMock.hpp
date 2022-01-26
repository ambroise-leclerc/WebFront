/// @date 26/01/2022 11:38:14
/// @author Ambroise Leclerc
/// @brief a Networking mock implementation for testing purposes
#pragma once
#include "BasicNetworking.hpp"

namespace webfront {
namespace networking {

class NetworkingMock : public BasicNetworking<> {
public:
    using super::ConstBuffer;
    using super::MutableBuffer;

    template<typename... Args>
    static auto AsyncWrite(Args&&... args) -> void {
        return;
    }
};

} // namespace networking
} // namespace webfront