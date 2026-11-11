// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief Gives every scenario the process-wide state a process of its own would have, so that the scenarios pass in any
/// order within one process (WEBFRONT_TEST_UNIT_SHARDS, #543).
#include <webfront/tooling/Logger.hpp>

#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <array>

namespace {

/// The enabled severities are global: a scenario that raises them makes the next one log, and a connected browser is
/// sent what is logged at the debug level.
class ScenarioIsolation : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override {
        for (std::size_t index = 0; index != severities.size(); ++index)
            initial[index] = webfront::log::is(severities[index]);
    }
    void testCaseStarting(const Catch::TestCaseInfo&) override {
        for (std::size_t index = 0; index != severities.size(); ++index)
            webfront::log::set(severities[index], initial[index]);
    }

private:
    static constexpr std::array severities{webfront::log::Error, webfront::log::Warn, webfront::log::Info, webfront::log::Debug};
    std::array<bool, 4>         initial{};
};

}  // namespace

CATCH_REGISTER_LISTENER(ScenarioIsolation)
