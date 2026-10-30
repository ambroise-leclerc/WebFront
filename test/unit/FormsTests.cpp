#include "FormsHarness.hpp"
#include "FormsSettings.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/components/forms/Form.hpp>
// Without C++26 reflection the optional layer compiles to nothing.
#include <webfront/components/forms/Reflection.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace webfront;
using forms_fixture::Pattern;
using forms_fixture::Recorder;
using forms_fixture::Settings;

namespace {
using Operations = std::vector<msg::DomOperation>;

// The attributes, properties or listeners that a batch sets on one node, by name.
std::map<std::string, std::string> on(const Recorder& recorder, const Operations& ops, const dom::Node& node, msg::DomOp code) {
    std::map<std::string, std::string> result;
    for (const auto& op : ops)
        if (op.code == code && op.a == node.id())
            result[recorder.name(op.b)] = code == msg::DomOp::listen ? std::to_string(op.c) : op.value;
    return result;
}

std::size_t count(const Recorder& recorder, const Operations& ops, std::string_view tag) {
    return static_cast<std::size_t>(std::ranges::count_if(ops, [&](const msg::DomOperation& op) {
        return op.code == msg::DomOp::element && recorder.name(op.b) == tag;
    }));
}

forms::Field<Settings>& fieldNamed(forms::Description<Settings>& description, std::string_view name) {
    return *std::ranges::find(description.fields, name, &forms::Field<Settings>::name);
}
}  // namespace

SCENARIO("Hand-written descriptors parse and format the values of the browser", "[forms]") {
    auto     description = forms_fixture::settingsDescription();
    Settings settings;
    GIVEN("the descriptor of each member kind") {
        THEN("names, labels, kinds, bounds and choices are those written") {
            REQUIRE(description.fields.size() == 5);
            const auto& speed = fieldNamed(description, "speed");
            REQUIRE(speed.label == "Speed");
            REQUIRE(speed.kind == forms::Kind::integer);
            REQUIRE(speed.range->min == 1);
            REQUIRE(speed.range->max == 10);
            REQUIRE(fieldNamed(description, "density").kind == forms::Kind::number);
            REQUIRE(fieldNamed(description, "wrap").kind == forms::Kind::boolean);
            REQUIRE(fieldNamed(description, "pattern").kind == forms::Kind::choice);
            REQUIRE(fieldNamed(description, "pattern").choices == std::vector<std::string>{"glider", "pulsar", "random"});
            REQUIRE(fieldNamed(description, "pattern").label == "pattern");
            REQUIRE(fieldNamed(description, "title").kind == forms::Kind::text);
            REQUIRE_NOTHROW(forms::validate(description));
        }
        THEN("read formats each member as the browser shows it, independently of the standard") {
            settings.density = 1.0;
            REQUIRE(fieldNamed(description, "speed").read(settings) == "5");
            REQUIRE(fieldNamed(description, "density").read(settings) == "1");
            REQUIRE(fieldNamed(description, "wrap").read(settings) == "true");
            REQUIRE(fieldNamed(description, "pattern").read(settings) == "glider");
            REQUIRE(fieldNamed(description, "title").read(settings) == "Life");
        }
        THEN("write accepts the values within bounds and choices") {
            REQUIRE(fieldNamed(description, "speed").write(settings, "10"));
            REQUIRE(fieldNamed(description, "density").write(settings, "1e-1"));
            REQUIRE(fieldNamed(description, "wrap").write(settings, "false"));
            REQUIRE(fieldNamed(description, "pattern").write(settings, "pulsar"));
            REQUIRE(fieldNamed(description, "title").write(settings, "été"));
            REQUIRE(settings.speed == 10);
            REQUIRE(settings.density == 0.1);
            REQUIRE_FALSE(settings.wrap);
            REQUIRE(settings.pattern == Pattern::pulsar);
            REQUIRE(settings.title == "été");
        }
        THEN("write refuses malformed, out-of-range or unknown values and keeps the member") {
            for (auto text : {"11", "0", "7.5", "", "abc", "5 ", "99999999999999999999"})
                REQUIRE_FALSE(fieldNamed(description, "speed").write(settings, text));
            for (auto text : {"1.5", "-0.1", "nan", "inf", "", "0,5", "1e400"})
                REQUIRE_FALSE(fieldNamed(description, "density").write(settings, text));
            REQUIRE_FALSE(fieldNamed(description, "wrap").write(settings, "on"));
            REQUIRE_FALSE(fieldNamed(description, "pattern").write(settings, "Glider"));
            REQUIRE(settings.speed == 5);
            REQUIRE(settings.density == 0.25);
            REQUIRE(settings.wrap);
            REQUIRE(settings.pattern == Pattern::glider);
        }
        THEN("an integer accepts the other forms that an integer input reports") {
            REQUIRE(fieldNamed(description, "speed").write(settings, "1e1"));
            REQUIRE(settings.speed == 10);
            REQUIRE(fieldNamed(description, "speed").write(settings, "5.0"));
            REQUIRE(settings.speed == 5);
            for (auto text : {"5.5", "1e-1", "1e400", "-1e20"})
                REQUIRE_FALSE(fieldNamed(description, "speed").write(settings, text));
        }
    }
    GIVEN("numbers that need every significant digit, and 64-bit integers beyond 2^53") {
        struct Precise {
            double       ratio{0.12345678901234567};
            std::int64_t count{};
        };
        constexpr double            ratioMax = 0.1234567890123456;
        forms::Description<Precise> precise{
            .name   = "Precise",
            .fields = {forms::field("ratio", &Precise::ratio, "Ratio", forms::Bounds{0, 0.5}),
                       forms::field("count", &Precise::count, "Count", forms::Bounds{0, 9007199254740992.0})}
        };
        Precise object;
        THEN("formatted values read back as the same number") {
            REQUIRE(std::stod(precise.fields[0].read(object)) == object.ratio);
            REQUIRE(forms::detail::formatNumber(ratioMax) == "0.1234567890123456");
            REQUIRE(forms::detail::formatNumber(0.1) == "0.1");
            REQUIRE(forms::detail::formatNumber(0.1f) == "0.1");
        }
        THEN("an integer is compared exactly with its bounds") {
            REQUIRE(precise.fields[1].write(object, "9007199254740992"));
            REQUIRE_FALSE(precise.fields[1].write(object, "9007199254740993"));
            REQUIRE(object.count == 9007199254740992);
            REQUIRE_FALSE(precise.fields[1].write(object, "-1"));
        }
    }
}

SCENARIO("Descriptors are validated before generation", "[forms]") {
    auto description = forms_fixture::settingsDescription();
    WHEN("a name is not an identifier or is repeated") {
        auto badType = description;
        badType.name = "Settings type";
        REQUIRE_THROWS_AS(forms::validate(badType), std::invalid_argument);
        auto badField           = description;
        badField.fields[0].name = "1speed";
        REQUIRE_THROWS_AS(forms::validate(badField), std::invalid_argument);
        auto repeated           = description;
        repeated.fields[1].name = "speed";
        REQUIRE_THROWS_AS(forms::typescriptDeclaration(repeated), std::invalid_argument);
    }
    WHEN("the type name is one TypeScript refuses for an interface") {
        for (auto name : {"number", "string", "any", "boolean", "var", "let", "interface"}) {
            auto reserved = description;
            reserved.name = name;
            REQUIRE_THROWS_AS(forms::validate(reserved), std::invalid_argument);
        }
    }
    WHEN("an integer has fractional bounds, which would shift the browser's steps") {
        auto fractional            = description;
        fractional.fields[0].range = forms::Bounds{0.5, 10};
        REQUIRE_THROWS_AS(forms::validate(fractional), std::invalid_argument);
        auto number            = description;
        number.fields[1].range = forms::Bounds{0.5, 0.75};
        REQUIRE_NOTHROW(forms::validate(number));
    }
    WHEN("bounds are inverted, not finite or on a member that is not a number") {
        auto inverted            = description;
        inverted.fields[0].range = forms::Bounds{10, 1};
        REQUIRE_THROWS_AS(forms::validate(inverted), std::invalid_argument);
        auto infinite            = description;
        infinite.fields[0].range = forms::Bounds{0, std::numeric_limits<double>::infinity()};
        REQUIRE_THROWS_AS(forms::validate(infinite), std::invalid_argument);
        auto text            = description;
        text.fields[4].range = forms::Bounds{0, 1};
        REQUIRE_THROWS_AS(forms::validate(text), std::invalid_argument);
    }
    WHEN("choices are missing, repeated or given to another kind, or an accessor is missing") {
        auto none              = description;
        none.fields[3].choices = {};
        REQUIRE_THROWS_AS(forms::validate(none), std::invalid_argument);
        auto repeated              = description;
        repeated.fields[3].choices = {"glider", "glider"};
        REQUIRE_THROWS_AS(forms::validate(repeated), std::invalid_argument);
        auto stray              = description;
        stray.fields[4].choices = {"a"};
        REQUIRE_THROWS_AS(forms::validate(stray), std::invalid_argument);
        auto unreadable           = description;
        unreadable.fields[2].read = {};
        REQUIRE_THROWS_AS(forms::validate(unreadable), std::invalid_argument);
    }
}

SCENARIO("A description generates the TypeScript declaration of its type", "[forms]") {
    GIVEN("the hand-written settings") {
        THEN("numbers, booleans, strings and the union of the choices are declared, with labels and bounds") {
            REQUIRE(forms::typescriptDeclaration(forms_fixture::settingsDescription()) == forms_fixture::settingsDeclaration);
        }
    }
    GIVEN("labels and choices that would break out of a comment or a string") {
        struct Odd {
            int  value{};
            bool flag{};
        };
        enum class Quote { plain, quoted };
        struct Choice {
            Quote quote{};
        };
        forms::Description<Odd> odd{
            .name   = "Odd",
            .fields = {forms::field("value", &Odd::value, "a */ b\nc"), forms::field("flag", &Odd::flag)}
        };
        forms::Description<Choice> choice{
            .name   = "Choice",
            .fields = {forms::choice("quote", &Choice::quote, {{"plain", Quote::plain}, {"say \"hi\"\\", Quote::quoted}})}
        };
        THEN("comments and strings stay closed") {
            REQUIRE(forms::typescriptDeclaration(odd)
                    == "export interface Odd {\n    /** a *\\/ b c */\n    readonly value: number;\n    readonly flag: boolean;\n}\n");
            REQUIRE(forms::typescriptDeclaration(choice) == "export interface Choice {\n    readonly quote: \"plain\" | \"say \\\"hi\\\"\\\\\";\n}\n");
        }
    }
}

SCENARIO("A form is built in a document and writes the object from browser events", "[forms][dom]") {
    Recorder recorder;
    auto     settings = std::make_shared<Settings>();
    auto     root     = recorder.doc.island();
    auto     form     = forms::form(recorder.doc, root, settings, forms_fixture::settingsDescription());
    auto     ops      = recorder.take();
    THEN("one batch holds a labelled input per member, with its value, type and bounds") {
        REQUIRE(count(recorder, ops, "form") == 1);
        REQUIRE(count(recorder, ops, "label") == 5);
        REQUIRE(count(recorder, ops, "input") == 4);
        REQUIRE(count(recorder, ops, "select") == 1);
        REQUIRE(count(recorder, ops, "option") == 3);
        using Attributes = std::map<std::string, std::string>;
        REQUIRE(on(recorder, ops, form.control("speed"), msg::DomOp::attribute)
                == Attributes{
                    { "type", "number"},
                    {"value",      "5"},
                    { "step",      "1"},
                    {  "min",      "1"},
                    {  "max",     "10"},
                    { "name",  "speed"}
        });
        REQUIRE(on(recorder, ops, form.control("density"), msg::DomOp::attribute)
                == Attributes{
                    { "type",  "number"},
                    {"value",    "0.25"},
                    { "step",     "any"},
                    {  "min",       "0"},
                    {  "max",       "1"},
                    { "name", "density"}
        });
        REQUIRE(on(recorder, ops, form.control("wrap"), msg::DomOp::attribute)
                == Attributes{
                    {   "type", "checkbox"},
                    {"checked",         ""},
                    {   "name",     "wrap"}
        });
        REQUIRE(on(recorder, ops, form.control("title"), msg::DomOp::attribute)
                == Attributes{
                    { "type",  "text"},
                    {"value",  "Life"},
                    { "name", "title"}
        });
        REQUIRE(on(recorder, ops, form.control("pattern"), msg::DomOp::attribute)
                == Attributes{
                    {"name", "pattern"}
        });
        REQUIRE(std::ranges::count_if(ops,
                                      [&](const auto& op) {
                                          return op.code == msg::DomOp::attribute && recorder.name(op.b) == "selected";
                                      })
                == 1);
        REQUIRE(on(recorder, ops, form.node(), msg::DomOp::listen)
                == Attributes{
                    {"submit", "1"}
        });
        REQUIRE(on(recorder, ops, form.control("speed"), msg::DomOp::listen)
                == Attributes{
                    {"input", "0"}
        });
        REQUIRE(on(recorder, ops, form.control("wrap"), msg::DomOp::listen)
                == Attributes{
                    {"change", "0"}
        });
        REQUIRE(on(recorder, ops, form.control("pattern"), msg::DomOp::listen)
                == Attributes{
                    {"change", "0"}
        });
        REQUIRE(ops.back().code == msg::DomOp::append);
        REQUIRE(ops.back().a == root.id());
        REQUIRE(ops.back().b == form.node().id());
    }
    WHEN("the browser sends valid values") {
        std::vector<std::string> changed;
        form.onChange([&](std::string_view name) {
            changed.emplace_back(name);
        });
        recorder.event(form.control("speed"), "input", "8");
        recorder.event(form.control("wrap"), "change", "false");
        recorder.event(form.control("pattern"), "change", "random");
        recorder.event(form.control("title"), "input", "Gosper");
        THEN("the members change and the application is told which") {
            REQUIRE(settings->speed == 8);
            REQUIRE_FALSE(settings->wrap);
            REQUIRE(settings->pattern == Pattern::random);
            REQUIRE(settings->title == "Gosper");
            REQUIRE(changed == std::vector<std::string>{"speed", "wrap", "pattern", "title"});
            REQUIRE(recorder.take().empty());
        }
    }
    WHEN("the change callback replaces itself while it runs") {
        std::vector<std::string> changed;
        auto                     tag = std::make_shared<std::string>("first");
        form.onChange([&, tag](std::string_view name) {
            form.onChange([&](std::string_view next) {
                changed.emplace_back("second:" + std::string(next));
            });
            // The replaced callback still runs, with its captures alive.
            changed.emplace_back(*tag + ":" + std::string(name));
        });
        recorder.event(form.control("speed"), "input", "8");
        recorder.event(form.control("speed"), "input", "7");
        THEN("the first call completes and the next change reaches the new callback") {
            REQUIRE(changed == std::vector<std::string>{"first:speed", "second:speed"});
        }
    }
    WHEN("the browser sends a refused value, then a valid one") {
        recorder.event(form.control("speed"), "input", "42");
        auto refused = recorder.take();
        recorder.event(form.control("speed"), "input", "42");
        auto repeated = recorder.take();
        recorder.event(form.control("speed"), "input", "3");
        auto accepted = recorder.take();
        THEN("the member keeps its value while the input is marked invalid") {
            REQUIRE(on(recorder, refused, form.control("speed"), msg::DomOp::attribute)
                    == std::map<std::string, std::string>{
                        {"aria-invalid", "true"}
            });
            REQUIRE(repeated.empty());
            REQUIRE(accepted.size() == 1);
            REQUIRE(accepted[0].code == msg::DomOp::unsetAttribute);
            REQUIRE(recorder.name(accepted[0].b) == "aria-invalid");
            REQUIRE(settings->speed == 3);
        }
    }
    WHEN("C++ changes the object and refreshes the form") {
        settings->speed   = 9;
        settings->wrap    = false;
        settings->pattern = Pattern::pulsar;
        settings->density = 0.5;
        form.refresh();
        auto refreshed = recorder.take();
        THEN("each input shows the new value through its property") {
            using Properties = std::map<std::string, std::string>;
            REQUIRE(on(recorder, refreshed, form.control("speed"), msg::DomOp::property)
                    == Properties{
                        {"value", "9"}
            });
            REQUIRE(on(recorder, refreshed, form.control("density"), msg::DomOp::property)
                    == Properties{
                        {"value", "0.5"}
            });
            REQUIRE(on(recorder, refreshed, form.control("wrap"), msg::DomOp::property)
                    == Properties{
                        {"checked", "false"}
            });
            REQUIRE(on(recorder, refreshed, form.control("pattern"), msg::DomOp::property)
                    == Properties{
                        {"selectedIndex", "1"}
            });
        }
    }
    WHEN("the form is removed after its handle is gone") {
        auto      other = std::make_shared<Settings>();
        dom::Node node;
        {
            auto second = forms::form(recorder.doc, root, other, forms_fixture::settingsDescription());
            node        = second.node();
        }
        REQUIRE(other.use_count() == 2);  // The listeners keep the form, and the object, alive.
        node.remove();
        THEN("its listeners, and their share of the object, are released") {
            REQUIRE(other.use_count() == 1);
            REQUIRE_FALSE(node.valid());
        }
    }
    WHEN("a choice member holds a value missing from its choices") {
        auto unlisted     = std::make_shared<Settings>();
        unlisted->pattern = static_cast<Pattern>(42);
        auto other        = forms::form(recorder.doc, root, unlisted, forms_fixture::settingsDescription());
        auto built        = recorder.take();
        THEN("no option is selected, as after a refresh, instead of the first one") {
            REQUIRE(std::ranges::none_of(built, [&](const auto& op) {
                return op.code == msg::DomOp::attribute && recorder.name(op.b) == "selected";
            }));
            REQUIRE(on(recorder, built, other.control("pattern"), msg::DomOp::property)
                    == std::map<std::string, std::string>{
                        {"selectedIndex", "-1"}
            });
        }
    }
    WHEN("a field name is unknown or the description is invalid") {
        REQUIRE_THROWS_AS(form.control("missing"), std::out_of_range);
        auto invalid = forms_fixture::settingsDescription();
        invalid.name = "";
        recorder.take();
        REQUIRE_THROWS_AS(forms::form(recorder.doc, root, settings, invalid), std::invalid_argument);
        REQUIRE_THROWS_AS(forms::form(recorder.doc, root, std::shared_ptr<Settings>{}, forms_fixture::settingsDescription()), std::invalid_argument);
        REQUIRE(recorder.take().empty());
    }
}

SCENARIO("An inspector shows an object and updates only what changed", "[forms][dom]") {
    Recorder recorder;
    auto     settings  = std::make_shared<Settings>();
    auto     inspector = forms::inspector(recorder.doc, recorder.doc.island(), settings, forms_fixture::settingsDescription());
    auto     ops       = recorder.take();
    THEN("a table row per member holds its label and its value as text") {
        REQUIRE(count(recorder, ops, "table") == 1);
        REQUIRE(count(recorder, ops, "tr") == 5);
        std::vector<std::string> texts;
        for (const auto& op : ops)
            if (op.code == msg::DomOp::text)
                texts.push_back(op.value);
        REQUIRE(texts == std::vector<std::string>{"Speed", "5", "Initial density", "0.25", "Wrap around edges", "true", "pattern", "glider", "title", "Life"});
    }
    WHEN("C++ changes one member and refreshes twice") {
        settings->title = "Gun";
        inspector.refresh();
        auto first = recorder.take();
        inspector.refresh();
        THEN("one text changes once") {
            REQUIRE(first.size() == 1);
            REQUIRE(first[0].code == msg::DomOp::setText);
            REQUIRE(first[0].a == inspector.value("title").id());
            REQUIRE(first[0].value == "Gun");
            REQUIRE(recorder.take().empty());
        }
    }
}
