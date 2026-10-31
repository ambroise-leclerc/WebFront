// Annotations that describe<T>() rejects at compile time (#383). CMake compiles this file once per REJECT_ macro and
// expects the message of the matching static_assert; without any macro it must compile.
#include <webfront/components/forms/Reflection.hpp>

#include <string>

using webfront::forms::Label;
using webfront::forms::Range;

enum class Pattern { glider, pulsar };

struct Annotated {
    [[= Label{"Speed"}, = Range{1, 10}]] int speed{5};
#if defined(REJECT_RANGE_ON_BOOL)
    [[= Range{0, 1}]] bool wrap{};
#elif defined(REJECT_RANGE_ON_STRING)
    [[= Range{0, 1}]] std::string title;
#elif defined(REJECT_RANGE_ON_ENUM)
    [[= Range{0, 1}]] Pattern pattern{};
#elif defined(REJECT_REPEATED_LABEL)
    [[= Label{"Density"}, = Label{"Initial density"}]] double density{};
#elif defined(REJECT_REPEATED_RANGE)
    [[= Range{0, 10}, = Range{1, 5}]] int count{};
#endif
};

int main() {
    return static_cast<int>(webfront::forms::describe<Annotated>().fields.empty());
}
