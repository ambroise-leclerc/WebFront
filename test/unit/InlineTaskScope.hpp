/// @brief Scope fixture for tasks whose mock scheduler and cancellation complete inline.
#pragma once
#include <webfront/networking/NetworkingMock.hpp>

namespace webfront::testing {
class InlineTaskScope : public networking::TaskScope {
    networking::IoContextMock context;

public:
    ~InlineTaskScope() {
        drainUntilJoined(context);
    }
};
}  // namespace webfront::testing
