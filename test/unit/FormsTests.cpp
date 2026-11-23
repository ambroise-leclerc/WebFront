#include "FormsHarness.hpp"
#include "FormsSettings.hpp"
#include <webfront/testing/Bdd.hpp>
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
#include <thread>
#include <utility>
#include <vector>

using namespace webfront::bdd;

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

const Scenario handWrittenDescriptors{"Hand-written descriptors parse and format the values of the browser", {"forms"}, [] {
    auto     description = forms_fixture::settingsDescription();
    Settings settings;
    given("the descriptor of each member kind", [&] {
        then("names, labels, kinds, bounds and choices are those written", [&] {
            require(description.fields.size() == 5);
            const auto& speed = fieldNamed(description, "speed");
            require(speed.label == "Speed");
            require(speed.kind == forms::Kind::integer);
            require(speed.range->min == 1);
            require(speed.range->max == 10);
            require(fieldNamed(description, "density").kind == forms::Kind::number);
            require(fieldNamed(description, "wrap").kind == forms::Kind::boolean);
            require(fieldNamed(description, "pattern").kind == forms::Kind::choice);
            require(fieldNamed(description, "pattern").choices == std::vector<std::string>{"glider", "pulsar", "random"});
            require(fieldNamed(description, "pattern").label == "pattern");
            require(fieldNamed(description, "title").kind == forms::Kind::text);
            requireNothrow([&] { forms::validate(description); });
        });
        then("read formats each member as the browser shows it, independently of the standard", [&] {
            settings.density = 1.0;
            require(fieldNamed(description, "speed").read(settings) == "5");
            require(fieldNamed(description, "density").read(settings) == "1");
            require(fieldNamed(description, "wrap").read(settings) == "true");
            require(fieldNamed(description, "pattern").read(settings) == "glider");
            require(fieldNamed(description, "title").read(settings) == "Life");
        });
        then("write accepts the values within bounds and choices", [&] {
            require(fieldNamed(description, "speed").write(settings, "10"));
            require(fieldNamed(description, "density").write(settings, "1e-1"));
            require(fieldNamed(description, "wrap").write(settings, "false"));
            require(fieldNamed(description, "pattern").write(settings, "pulsar"));
            require(fieldNamed(description, "title").write(settings, "été"));
            require(settings.speed == 10);
            require(settings.density == 0.1);
            require(!settings.wrap);
            require(settings.pattern == Pattern::pulsar);
            require(settings.title == "été");
        });
        then("write refuses malformed, out-of-range or unknown values and keeps the member", [&] {
            for (auto text : {"11", "0", "7.5", "", "abc", "5 ", "99999999999999999999"})
                require(!fieldNamed(description, "speed").write(settings, text));
            for (auto text : {"1.5", "-0.1", "nan", "inf", "", "0,5", "1e400"})
                require(!fieldNamed(description, "density").write(settings, text));
            require(!fieldNamed(description, "wrap").write(settings, "on"));
            require(!fieldNamed(description, "pattern").write(settings, "Glider"));
            require(settings.speed == 5);
            require(settings.density == 0.25);
            require(settings.wrap);
            require(settings.pattern == Pattern::glider);
        });
        then("an integer accepts the other forms that an integer input reports", [&] {
            require(fieldNamed(description, "speed").write(settings, "1e1"));
            require(settings.speed == 10);
            require(fieldNamed(description, "speed").write(settings, "5.0"));
            require(settings.speed == 5);
            for (auto text : {"5.5", "1e-1", "1e400", "-1e20"})
                require(!fieldNamed(description, "speed").write(settings, text));
        });
    });
    given("numbers that need every significant digit, and 64-bit integers beyond 2^53", [&] {
        struct Precise {
            double       ratio{0.12345678901234567};
            std::int64_t count{};
        };
        static constexpr double            ratioMax = 0.1234567890123456;
        forms::Description<Precise> precise{
            .name   = "Precise",
            .fields = {forms::field("ratio", &Precise::ratio, "Ratio", forms::Bounds{0, 0.5}),
                       forms::field("count", &Precise::count, "Count", forms::Bounds{0, 9007199254740992.0})}
        };
        Precise object;
        then("formatted values read back as the same number", [&] {
            require(std::stod(precise.fields[0].read(object)) == object.ratio);
            require(forms::detail::formatNumber(ratioMax) == "0.1234567890123456");
            require(forms::detail::formatNumber(0.1) == "0.1");
            require(forms::detail::formatNumber(0.1f) == "0.1");
        });
        then("an integer is compared exactly with its bounds", [&] {
            require(precise.fields[1].write(object, "9007199254740992"));
            require(!precise.fields[1].write(object, "9007199254740993"));
            require(object.count == 9007199254740992);
            require(!precise.fields[1].write(object, "-1"));
        });
        then("a value that underflows is refused whatever the standard library, and an exact zero is accepted", [&] {
            for (auto text : {"1e-320", "1e-400", "-2.5e-310"})
                require(!precise.fields[0].write(object, text));
            require(object.ratio == 0.12345678901234567);
            require(precise.fields[0].write(object, "0e-400"));
            require(object.ratio == 0);
        });
    });
    given("float members whose decimal bounds are not exact in binary", [&] {
        struct Gains {
            float gain{};
            float wide{};
        };
        forms::Description<Gains> gains{
            .name   = "Gains",
            .fields = {forms::field("gain", &Gains::gain, "Gain", forms::Bounds{0.1, 0.3}),
                       forms::field("wide", &Gains::wide, "Wide", forms::Bounds{-1e300, 1e300})}
        };
        Gains object;
        then("the bounds the form shows are accepted, and only what lies beyond them is refused", [&] {
            require(gains.fields[0].write(object, "0.1"));
            require(object.gain == 0.1f);
            require(gains.fields[0].write(object, "0.3"));
            require(object.gain == 0.3f);
            for (auto text : {"0.0999", "0.3001", "1e-40"})
                require(!gains.fields[0].write(object, text));
            require(object.gain == 0.3f);
        });
        then("bounds beyond the range of float accept every finite float", [&] {
            require(gains.fields[1].write(object, "-3e38"));
            require(gains.fields[1].write(object, "3e38"));
            require(!gains.fields[1].write(object, "4e38"));
        });
    });
}};

const Scenario descriptorsValidated{"Descriptors are validated before generation", {"forms"}, [] {
    auto description = forms_fixture::settingsDescription();
    when("a name is not an identifier or is repeated", [&] {
        auto badType = description;
        badType.name = "Settings type";
        requireThrows<std::invalid_argument>([&] { forms::validate(badType); });
        auto badField           = description;
        badField.fields[0].name = "1speed";
        requireThrows<std::invalid_argument>([&] { forms::validate(badField); });
        auto repeated           = description;
        repeated.fields[1].name = "speed";
        requireThrows<std::invalid_argument>([&] { forms::typescriptDeclaration(repeated); });
    });
    when("the type name is one TypeScript refuses for an interface", [&] {
        for (auto name : {"number", "string", "any", "boolean", "var", "let", "interface", "eval", "arguments"}) {
            auto reserved = description;
            reserved.name = name;
            requireThrows<std::invalid_argument>([&] { forms::validate(reserved); });
        }
    });
    when("an integer has fractional bounds, which would shift the browser's steps", [&] {
        auto fractional            = description;
        fractional.fields[0].range = forms::Bounds{0.5, 10};
        requireThrows<std::invalid_argument>([&] { forms::validate(fractional); });
        auto number            = description;
        number.fields[1].range = forms::Bounds{0.5, 0.75};
        requireNothrow([&] { forms::validate(number); });
    });
    when("bounds are inverted, not finite or on a member that is not a number", [&] {
        auto inverted            = description;
        inverted.fields[0].range = forms::Bounds{10, 1};
        requireThrows<std::invalid_argument>([&] { forms::validate(inverted); });
        auto infinite            = description;
        infinite.fields[0].range = forms::Bounds{0, std::numeric_limits<double>::infinity()};
        requireThrows<std::invalid_argument>([&] { forms::validate(infinite); });
        auto text            = description;
        text.fields[4].range = forms::Bounds{0, 1};
        requireThrows<std::invalid_argument>([&] { forms::validate(text); });
    });
    when("choices are missing, repeated or given to another kind, or an accessor is missing", [&] {
        auto none              = description;
        none.fields[3].choices = {};
        requireThrows<std::invalid_argument>([&] { forms::validate(none); });
        auto repeated              = description;
        repeated.fields[3].choices = {"glider", "glider"};
        requireThrows<std::invalid_argument>([&] { forms::validate(repeated); });
        auto stray              = description;
        stray.fields[4].choices = {"a"};
        requireThrows<std::invalid_argument>([&] { forms::validate(stray); });
        auto unreadable           = description;
        unreadable.fields[2].read = {};
        requireThrows<std::invalid_argument>([&] { forms::validate(unreadable); });
    });
}};

const Scenario descriptionGenerates{"A description generates the TypeScript declaration of its type", {"forms"}, [] {
    given("the hand-written settings", [&] {
        then("numbers, booleans, strings and the union of the choices are declared, with labels and bounds", [&] {
            require(forms::typescriptDeclaration(forms_fixture::settingsDescription()) == forms_fixture::settingsDeclaration);
        });
    });
    given("labels and choices that would break out of a comment or a string", [&] {
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
        then("comments and strings stay closed", [&] {
            require(forms::typescriptDeclaration(odd)
                    == "export interface Odd {\n    /** a *\\/ b c */\n    readonly value: number;\n    readonly flag: boolean;\n}\n");
            require(forms::typescriptDeclaration(choice) == "export interface Choice {\n    readonly quote: \"plain\" | \"say \\\"hi\\\"\\\\\";\n}\n");
        });
    });
}};

const Scenario formBuilt{"A form is built in a document and writes the object from browser events", {"forms", "dom"}, [] {
    Recorder recorder;
    auto     settings = std::make_shared<Settings>();
    auto     root     = recorder.doc.island();
    auto     form     = forms::form(recorder.doc, root, settings, forms_fixture::settingsDescription());
    auto     ops      = recorder.take();
    then("one batch holds a labelled input per member, with its value, type and bounds", [&] {
        require(count(recorder, ops, "form") == 1);
        require(count(recorder, ops, "label") == 5);
        require(count(recorder, ops, "input") == 4);
        require(count(recorder, ops, "select") == 1);
        require(count(recorder, ops, "option") == 3);
        using Attributes = std::map<std::string, std::string>;
        require(on(recorder, ops, form.control("speed"), msg::DomOp::attribute)
                == Attributes{
                    {"type", "number"},
                    {"step",      "1"},
                    { "min",      "1"},
                    { "max",     "10"},
                    {"name",  "speed"}
        });
        require(on(recorder, ops, form.control("density"), msg::DomOp::attribute)
                == Attributes{
                    {"type",  "number"},
                    {"step",     "any"},
                    { "min",       "0"},
                    { "max",       "1"},
                    {"name", "density"}
        });
        require(on(recorder, ops, form.control("wrap"), msg::DomOp::attribute)
                == Attributes{
                    {"type", "checkbox"},
                    {"name",     "wrap"}
        });
        require(on(recorder, ops, form.control("title"), msg::DomOp::attribute)
                == Attributes{
                    {"type",  "text"},
                    {"name", "title"}
        });
        require(on(recorder, ops, form.control("pattern"), msg::DomOp::attribute)
                == Attributes{
                    {"name", "pattern"}
        });
        // The values are shown through the same properties as refresh().
        require(on(recorder, ops, form.control("speed"), msg::DomOp::property)
                == Attributes{
                    {"value", "5"}
        });
        require(on(recorder, ops, form.control("density"), msg::DomOp::property)
                == Attributes{
                    {"value", "0.25"}
        });
        require(on(recorder, ops, form.control("wrap"), msg::DomOp::property)
                == Attributes{
                    {"checked", "true"}
        });
        require(on(recorder, ops, form.control("title"), msg::DomOp::property)
                == Attributes{
                    {"value", "Life"}
        });
        require(on(recorder, ops, form.control("pattern"), msg::DomOp::property)
                == Attributes{
                    {"selectedIndex", "0"}
        });
        require(on(recorder, ops, form.node(), msg::DomOp::listen)
                == Attributes{
                    {"submit", "1"}
        });
        require(on(recorder, ops, form.control("speed"), msg::DomOp::listen)
                == Attributes{
                    {"input", "0"}
        });
        require(on(recorder, ops, form.control("wrap"), msg::DomOp::listen)
                == Attributes{
                    {"change", "0"}
        });
        require(on(recorder, ops, form.control("pattern"), msg::DomOp::listen)
                == Attributes{
                    {"change", "0"}
        });
        require(ops.back().code == msg::DomOp::append);
        require(ops.back().a == root.id());
        require(ops.back().b == form.node().id());
    });
    when("the browser sends valid values", [&] {
        std::vector<std::string> changed;
        form.onChange([&](std::string_view name) {
            changed.emplace_back(name);
        });
        recorder.event(form.control("speed"), "input", "8");
        recorder.event(form.control("wrap"), "change", "false");
        recorder.event(form.control("pattern"), "change", "random");
        recorder.event(form.control("title"), "input", "Gosper");
        then("the members change and the application is told which", [&] {
            require(settings->speed == 8);
            require(!settings->wrap);
            require(settings->pattern == Pattern::random);
            require(settings->title == "Gosper");
            require(changed == std::vector<std::string>{"speed", "wrap", "pattern", "title"});
            require(recorder.take().empty());
        });
    });
    when("the change callback replaces itself while it runs", [&] {
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
        then("the first call completes and the next change reaches the new callback", [&] {
            require(changed == std::vector<std::string>{"first:speed", "second:speed"});
        });
    });
    when("the browser sends a refused value, then a valid one", [&] {
        recorder.event(form.control("speed"), "input", "42");
        auto refused = recorder.take();
        recorder.event(form.control("speed"), "input", "42");
        auto repeated = recorder.take();
        recorder.event(form.control("speed"), "input", "3");
        auto accepted = recorder.take();
        then("the member keeps its value while the input is marked invalid", [&] {
            require(on(recorder, refused, form.control("speed"), msg::DomOp::attribute)
                    == std::map<std::string, std::string>{
                        {"aria-invalid", "true"}
            });
            require(repeated.empty());
            require(accepted.size() == 1);
            require(accepted[0].code == msg::DomOp::unsetAttribute);
            require(recorder.name(accepted[0].b) == "aria-invalid");
            require(settings->speed == 3);
        });
    });
    when("C++ changes the object and refreshes the form", [&] {
        settings->speed   = 9;
        settings->wrap    = false;
        settings->pattern = Pattern::pulsar;
        settings->density = 0.5;
        form.refresh();
        auto refreshed = recorder.take();
        then("each input shows the new value through its property", [&] {
            using Properties = std::map<std::string, std::string>;
            require(on(recorder, refreshed, form.control("speed"), msg::DomOp::property)
                    == Properties{
                        {"value", "9"}
            });
            require(on(recorder, refreshed, form.control("density"), msg::DomOp::property)
                    == Properties{
                        {"value", "0.5"}
            });
            require(on(recorder, refreshed, form.control("wrap"), msg::DomOp::property)
                    == Properties{
                        {"checked", "false"}
            });
            require(on(recorder, refreshed, form.control("pattern"), msg::DomOp::property)
                    == Properties{
                        {"selectedIndex", "1"}
            });
        });
    });
    when("the form is removed after its handle is gone", [&] {
        auto      other = std::make_shared<Settings>();
        dom::Node node;
        {
            auto second = forms::form(recorder.doc, root, other, forms_fixture::settingsDescription());
            node        = second.node();
        }
        require(other.use_count() == 2);  // The listeners keep the form, and the object, alive.
        node.remove();
        then("its listeners, and their share of the object, are released", [&] {
            require(other.use_count() == 1);
            require(!node.valid());
        });
    });
    when("a choice member holds a value missing from its choices", [&] {
        auto unlisted     = std::make_shared<Settings>();
        unlisted->pattern = static_cast<Pattern>(42);
        auto other        = forms::form(recorder.doc, root, unlisted, forms_fixture::settingsDescription());
        auto built        = recorder.take();
        then("no option is selected, as after a refresh, instead of the first one", [&] {
            require(std::ranges::none_of(built, [&](const auto& op) {
                return op.code == msg::DomOp::attribute && recorder.name(op.b) == "selected";
            }));
            require(on(recorder, built, other.control("pattern"), msg::DomOp::property)
                    == std::map<std::string, std::string>{
                        {"selectedIndex", "-1"}
            });
        });
    });
    when("an accessor throws while a form is built", [&] {
        auto failing           = forms_fixture::settingsDescription();
        failing.fields[2].read = [](const Settings&) -> std::string {
            throw std::runtime_error("unreadable");
        };
        const auto nodes     = recorder.state->nodes.size();
        const auto listeners = recorder.state->listeners.size();
        auto       other     = std::make_shared<Settings>();
        requireThrows<std::runtime_error>([&] { forms::form(recorder.doc, root, other, failing); });
        then("the elements already created are removed with their listeners and their share of the object", [&] {
            require(recorder.state->nodes.size() == nodes);
            require(recorder.state->listeners.size() == listeners);
            require(other.use_count() == 1);
        });
    });
    when("onChange is called from another thread", [&] {
        bool        refused = false;
        std::thread worker([&] {
            try {
                form.onChange([](std::string_view) { });
            } catch (const std::logic_error&) {
                refused = true;
            }
        });
        worker.join();
        then("it throws like the other methods instead of racing with event dispatch", [&] {
            require(refused);
        });
    });
    when("a field name is unknown or the description is invalid", [&] {
        requireThrows<std::out_of_range>([&] { form.control("missing"); });
        auto invalid = forms_fixture::settingsDescription();
        invalid.name = "";
        recorder.take();
        requireThrows<std::invalid_argument>([&] { forms::form(recorder.doc, root, settings, invalid); });
        requireThrows<std::invalid_argument>([&] { forms::form(recorder.doc, root, std::shared_ptr<Settings>{}, forms_fixture::settingsDescription()); });
        require(recorder.take().empty());
    });
}};

const Scenario inspectorShows{"An inspector shows an object and updates only what changed", {"forms", "dom"}, [] {
    Recorder recorder;
    auto     settings  = std::make_shared<Settings>();
    auto     inspector = forms::inspector(recorder.doc, recorder.doc.island(), settings, forms_fixture::settingsDescription());
    auto     ops       = recorder.take();
    then("a table row per member holds its label and its value as text", [&] {
        require(count(recorder, ops, "table") == 1);
        require(count(recorder, ops, "tr") == 5);
        std::vector<std::string> texts;
        for (const auto& op : ops)
            if (op.code == msg::DomOp::text)
                texts.push_back(op.value);
        require(texts == std::vector<std::string>{"Speed", "5", "Initial density", "0.25", "Wrap around edges", "true", "pattern", "glider", "title", "Life"});
    });
    when("C++ changes one member and refreshes twice", [&] {
        settings->title = "Gun";
        inspector.refresh();
        auto first = recorder.take();
        inspector.refresh();
        then("one text changes once", [&] {
            require(first.size() == 1);
            require(first[0].code == msg::DomOp::setText);
            require(first[0].a == inspector.value("title").id());
            require(first[0].value == "Gun");
            require(recorder.take().empty());
        });
    });
    when("an accessor throws while another inspector is built", [&] {
        auto failing           = forms_fixture::settingsDescription();
        failing.fields[3].read = [](const Settings&) -> std::string {
            throw std::runtime_error("unreadable");
        };
        const auto nodes = recorder.state->nodes.size();
        requireThrows<std::runtime_error>([&] { forms::inspector(recorder.doc, recorder.doc.island(), settings, failing); });
        then("its elements are removed", [&] {
            require(recorder.state->nodes.size() == nodes);
        });
    });
}};
