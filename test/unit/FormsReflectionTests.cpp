// The optional C++26 layer of the forms (#383), compiled only with reflection (GCC 16: -std=c++26 -freflection): the
// descriptors that describe<T>() reflects from annotations must equal the hand-written C++23 ones.
#include "FormsHarness.hpp"
#include "FormsSettings.hpp"
#include <webfront/testing/Bdd.hpp>
#include <webfront/components/forms/Form.hpp>
#include <webfront/components/forms/Reflection.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace webfront::bdd;

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

const Scenario reflectionProduces{"Reflection produces the descriptors written by hand", {"forms", "reflection"}, [] {
    const auto reflectedDescription = forms::describe<reflected::Settings>();
    const auto handWritten          = forms_fixture::settingsDescription();
    then("names, labels, kinds, bounds and choices match, private members aside", [&] {
        require(reflectedDescription.name == handWritten.name);
        require(reflectedDescription.fields.size() == handWritten.fields.size());
        for (std::size_t i = 0; i < handWritten.fields.size(); ++i) {
            const auto& r = reflectedDescription.fields[i];
            const auto& h = handWritten.fields[i];
            require(r.name == h.name);
            require(r.label == h.label);
            require(r.kind == h.kind);
            require(r.range.has_value() == h.range.has_value());
            if (h.range) {
                require(r.range->min == h.range->min);
                require(r.range->max == h.range->max);
            }
            require(r.choices == h.choices);
        }
    });
    then("the TypeScript declaration is the one wf check verifies", [&] {
        require(forms::typescriptDeclaration(reflectedDescription) == forms_fixture::settingsDeclaration);
    });
    then("the accessors format and parse like the hand-written ones", [&] {
        reflected::Settings settings;
        auto&               speed = reflectedDescription.fields[0];
        require(speed.read(settings) == "5");
        require(!speed.write(settings, "11"));
        require(speed.write(settings, "10"));
        require(settings.speed == 10);
        require(reflectedDescription.fields[1].read(settings) == "0.25");
        require(reflectedDescription.fields[3].write(settings, "pulsar"));
        require(settings.pattern == reflected::Pattern::pulsar);
    });
}};

const Scenario reflectedForm{"A reflected form is the hand-written form", {"forms", "reflection", "dom"}, [] {
    forms_fixture::Recorder byHand, byReflection;
    auto                    handSettings      = std::make_shared<forms_fixture::Settings>();
    auto                    reflectedSettings = std::make_shared<reflected::Settings>();
    forms::form(byHand.doc, byHand.doc.island(), handSettings, forms_fixture::settingsDescription());
    auto form = forms::form(byReflection.doc, byReflection.doc.island(), reflectedSettings, forms::describe<reflected::Settings>());
    then("both send the same batch", [&] {
        require(byReflection.take() == byHand.take());
    });
    when("the browser edits the reflected form", [&] {
        byReflection.event(form.control("density"), "input", "0.75");
        byReflection.event(form.control("density"), "input", "2");
        then("valid values reach the member and refused ones do not", [&] {
            require(reflectedSettings->density == 0.75);
        });
    });
}};
