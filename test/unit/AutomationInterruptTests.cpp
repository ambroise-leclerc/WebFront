// The interrupt handler must be safe as the first Windows include, before the socket declarations.
// clang-format off
#include <webfront/automation/Interrupt.hpp>
#include <webfront/automation/Channel.hpp>
// clang-format on
#include "Bdd.hpp"

using namespace webfront::bdd;

const Scenario nativeInterrupt{"Native interrupt handlers record catchable signals without doing shutdown work", [] {
    webfront::automation::Interrupt interrupt;
    require(interrupt.signal() == 0);
    require(std::raise(SIGINT) == 0);
    require(interrupt.signal() == SIGINT);
    require(std::raise(SIGTERM) == 0);
    require(interrupt.signal() == SIGTERM);
}};
