/// @brief Preserve native shutdown reasons while classifying a failed developer bootstrap.
#pragma once

#include <atomic>

namespace webfront::automation {
inline void recordBootstrapFailure(std::atomic<int>& status) {
    auto previous = status.load();
    // The transport may already have reported the closed session as a generic failure.
    // Once 70 is recorded, its compare-exchange from zero cannot replace it with 1.
    // A concurrent native deadline or signal retains its more specific exit status.
    while (previous == 0 || previous == 1) {
        if (status.compare_exchange_weak(previous, 70))
            return;
    }
}
}  // namespace webfront::automation
