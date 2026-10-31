// The optional C++26 layer of the forms (#383), compiled only with reflection (GCC 16: -std=c++26 -freflection): the
// descriptors that describe<T>() reflects from annotations must equal the hand-written C++23 ones.
#include "FormsHarness.hpp"
#include "FormsSettings.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/components/forms/Form.hpp>
#include <webfront/components/forms/Reflection.hpp>

#include <memory>
#include <string>
#include <vector>

#if !defined(__cpp_impl_reflection) || !defined(__cpp_expansion_statements)
    #error "This test needs C++26 reflection and expansion statements"
#endif

using namespace webfront;
using forms::Label;
using forms::Range;

namespace reflected {
enum class Pattern { glider, pulsar, random };

// The members of forms_fixture::Settings, annotated instead of described by hand.
struct Settings {
    [[= Label{"Speed"}, = Range{1, 10}]] int             speed{5};
    [[= Label{"Initial density"}, = Range{0, 1}]] double density{0.25};
    [[= Label{"Wrap around edges"}]] bool                wrap{true};
    Pattern                                              pattern{Pattern::glider};
    std::string                                          title{"Life"};

private:
    int hidden{};  // Not public: not described.
};
}  // namespace reflected

SCENARIO("Reflection produces the descriptors written by hand", "[forms][reflection]") {
    const auto reflectedDescription = forms::describe<reflected::Settings>();
    const auto handWritten          = forms_fixture::settingsDescription();
    THEN("names, labels, kinds, bounds and choices match, private members aside") {
        REQUIRE(reflectedDescription.name == handWritten.name);
        REQUIRE(reflectedDescription.fields.size() == handWritten.fields.size());
        for (std::size_t i = 0; i < handWritten.fields.size(); ++i) {
            const auto& r = reflectedDescription.fields[i];
            const auto& h = handWritten.fields[i];
            REQUIRE(r.name == h.name);
            REQUIRE(r.label == h.label);
            REQUIRE(r.kind == h.kind);
            REQUIRE(r.range.has_value() == h.range.has_value());
            if (h.range) {
                REQUIRE(r.range->min == h.range->min);
                REQUIRE(r.range->max == h.range->max);
            }
            REQUIRE(r.choices == h.choices);
        }
    }
    THEN("the TypeScript declaration is the one wf check verifies") {
        REQUIRE(forms::typescriptDeclaration(reflectedDescription) == forms_fixture::settingsDeclaration);
    }
    THEN("the accessors format and parse like the hand-written ones") {
        reflected::Settings settings;
        auto&               speed = reflectedDescription.fields[0];
        REQUIRE(speed.read(settings) == "5");
        REQUIRE_FALSE(speed.write(settings, "11"));
        REQUIRE(speed.write(settings, "10"));
        REQUIRE(settings.speed == 10);
        REQUIRE(reflectedDescription.fields[1].read(settings) == "0.25");
        REQUIRE(reflectedDescription.fields[3].write(settings, "pulsar"));
        REQUIRE(settings.pattern == reflected::Pattern::pulsar);
    }
}

SCENARIO("A reflected form is the hand-written form", "[forms][reflection][dom]") {
    forms_fixture::Recorder byHand, byReflection;
    auto                    handSettings      = std::make_shared<forms_fixture::Settings>();
    auto                    reflectedSettings = std::make_shared<reflected::Settings>();
    forms::form(byHand.doc, byHand.doc.island(), handSettings, forms_fixture::settingsDescription());
    auto form = forms::form(byReflection.doc, byReflection.doc.island(), reflectedSettings, forms::describe<reflected::Settings>());
    THEN("both send the same batch") {
        REQUIRE(byReflection.take() == byHand.take());
    }
    WHEN("the browser edits the reflected form") {
        byReflection.event(form.control("density"), "input", "0.75");
        byReflection.event(form.control("density"), "input", "2");
        THEN("valid values reach the member and refused ones do not") {
            REQUIRE(reflectedSettings->density == 0.75);
        }
    }
}
