/** @brief Ordinary consumer: the facade header supplies its complete API without imports. */
#include "tooling/Logger.hpp"

int main() {
    namespace log = webfront::log;
    log::setLogLevel(log::Info);
    unsigned int deliveries = 0;
    bool         valid      = true;
    const auto   handles    = log::addSinks(
        [&](std::string_view text) {  // NOLINT(misc-include-cleaner): verify that Logger.hpp alone supplies the facade API.
            ++deliveries;
            valid = valid && text.ends_with(" | answer 42");
        },
        [&](std::string_view) {
            ++deliveries;
        });
    log::info("answer {}", 42);
    log::removeSinks(handles[0]);
    log::info("answer {}", 43);
    log::removeSinks(handles[1]);
    log::setLogLevel(log::Disabled);
    log::info("invalid {");
    return valid && deliveries == 3 ? 0 : 1;
}
