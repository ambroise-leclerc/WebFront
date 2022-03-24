/// @date 08/03/2022 18:23:42
/// @author Ambroise Leclerc
/// @brief Return values management with futures values
#pragma once

#include <future>
#include <vector>

namespace webfront {
    
class JsReturnValue {
    std::future<std::vector<uint8_t>> rawValue;

public:
    void setFuture(std::future<std::vector<uint8_t>>&& futureValue) {
        rawValue = std::move(futureValue);
    }

    enum class WaitStatus { ready, timeout };

    template<typename Rep, typename Period>
    WaitStatus wait_for(std::chrono::duration<Rep,Period> timeout) const {
        return rawValue.wait_for(timeout) == std::future_status::ready ? WaitStatus::ready : WaitStatus::timeout;
    }

    template<typename Rep, typename Period>
    WaitStatus wait_for(std::chrono::time_point<Rep,Period> timePoint) const {
        return rawValue.wait_until(timePoint) == std::future_status::ready ? WaitStatus::ready : WaitStatus::timeout;
    }

    void wait() const {
        rawValue.wait();
    }

    template<typename T>
    T get() const {
        wait();

        T value;
 //       vérifier que le type encodé est convertible vers le type demandé et retourner la valeur
        
        return value;
    }
};

}