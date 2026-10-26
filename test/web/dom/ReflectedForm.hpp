// ADR-018 (#378): throwaway prototype of a user interface built from a C++ type with C++26 static reflection (P2996,
// annotations P3394, expansion statements P1306, define_static_* P3491). Never part of the library, which stays C++23.
// From an aggregate it builds a form in a Document (DomPrototype.hpp) whose inputs write the members back, a TypeScript
// declaration of the type, and a JSON image of a value. GCC 16.1 (-std=c++26 -freflection) compiles it.
#pragma once

#if defined(__cpp_impl_reflection)

    #include "DomPrototype.hpp"

    #include <charconv>
    #include <meta>
    #include <optional>
    #include <string>
    #include <string_view>
    #include <type_traits>

namespace dom_prototype::reflected {

// Annotations of the members: [[=Label{"Speed"}]] stores a static string (a string literal is not a structural value).
struct Label {
    const char* text;
    consteval Label(const char* value) : text(std::define_static_string(std::string_view(value))) {}
};
struct Range {
    double min;
    double max;
};

template <typename Annotation, std::meta::info member>
consteval std::optional<Annotation> annotation() {
    for (auto value : std::meta::annotations_of_with_type(member, ^^Annotation))
        return std::meta::extract<Annotation>(value);
    return std::nullopt;
}

template <typename T>
consteval auto members() {
    return std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
}

template <typename E>
std::string_view enumeratorName(E value) {
    template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^E))) {
        if (value == [:e:])
            return std::meta::identifier_of(e);
    }
    return {};
}

template <typename E>
bool parseEnumerator(std::string_view text, E& value) {
    template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^E))) {
        if (text == std::meta::identifier_of(e)) {
            value = [:e:];
            return true;
        }
    }
    return false;
}

template <typename F>
std::string format(const F& value) {
    if constexpr (std::is_same_v<F, bool>)
        return value ? "true" : "false";
    else if constexpr (std::is_enum_v<F>)
        return std::string(enumeratorName(value));
    else if constexpr (std::is_arithmetic_v<F>)
        return std::to_string(value);
    else
        return std::string(value);
}

// Writes the text of an input into a member; a value that does not parse, or that is outside its Range, is ignored.
template <typename F>
bool parse(std::string_view text, F& value, std::optional<Range> range) {
    if constexpr (std::is_same_v<F, bool>) {
        value = text == "true";
        return true;
    } else if constexpr (std::is_enum_v<F>) {
        return parseEnumerator(text, value);
    } else if constexpr (std::is_arithmetic_v<F>) {
        F parsed{};
        if (std::from_chars(text.data(), text.data() + text.size(), parsed).ec != std::errc{})
            return false;
        if (range && (parsed < range->min || parsed > range->max))
            return false;
        value = parsed;
        return true;
    } else {
        value = std::string(text);
        return true;
    }
}

/// One labelled input per member, appended to parent; the object must outlive the form's handlers.
template <typename T, typename UI>
void buildForm(Document<UI>& doc, Node<UI> parent, T& object) {
    template for (constexpr auto member : members<T>()) {
        constexpr std::string_view name  = std::meta::identifier_of(member);
        constexpr auto             label = annotation<Label, member>();
        constexpr auto             range = annotation<Range, member>();
        auto&                      value = object.[:member:];
        using F                          = std::remove_cvref_t<decltype(value)>;

        auto row = doc.element("label");
        row.attr("class", "field").text(label ? std::string_view(label->text) : name);
        if constexpr (std::is_enum_v<F>) {
            auto select = doc.element("select");
            select.attr("name", name);
            template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^F))) {
                auto option = doc.element("option");
                option.attr("value", std::meta::identifier_of(e)).text(std::meta::identifier_of(e));
                if (value == [:e:])
                    option.attr("selected", "");
                select.child(option);
            }
            select.on("change", [&value](std::string_view text) {
                parse(text, value, std::nullopt);
            });
            row.child(select);
        } else {
            auto input = doc.element("input");
            input.attr("name", name);
            if constexpr (std::is_same_v<F, bool>) {
                input.attr("type", "checkbox");
                if (value)
                    input.attr("checked", "");
            } else if constexpr (std::is_arithmetic_v<F>) {
                input.attr("type", "number").attr("value", format(value));
                if constexpr (range.has_value())
                    input.attr("min", std::to_string(range->min)).attr("max", std::to_string(range->max));
            } else {
                input.attr("type", "text").attr("value", value);
            }
            input.on(std::is_same_v<F, bool> ? "change" : "input", [&value, bounds = range](std::string_view text) {
                parse(text, value, bounds);
            });
            row.child(input);
        }
        parent.child(row);
    }
}

template <typename F>
std::string typescriptType() {
    if constexpr (std::is_same_v<F, bool>)
        return "boolean";
    else if constexpr (std::is_enum_v<F>) {
        std::string result;
        template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^F))) {
            if (!result.empty())
                result += " | ";
            result += '"';
            result += std::meta::identifier_of(e);
            result += '"';
        }
        return result;
    } else if constexpr (std::is_arithmetic_v<F>)
        return "number";
    else
        return "string";
}

/// The declaration a TypeScript UI would import to read the same type (generated, never written by hand).
template <typename T>
std::string typescriptDeclaration() {
    std::string result = "export interface " + std::string(std::meta::identifier_of(^^T)) + " {\n";
    template for (constexpr auto member : members<T>()) {
        using F = std::remove_cvref_t<decltype(std::declval<T&>().[:member:])>;
        result += "    readonly " + std::string(std::meta::identifier_of(member)) + ": " + typescriptType<F>() + ";\n";
    }
    return result + "}\n";
}

template <typename T>
std::string toJson(const T& object) {
    std::string result = "{";
    template for (constexpr auto member : members<T>()) {
        const auto& value = object.[:member:];
        using F           = std::remove_cvref_t<decltype(value)>;
        if (result.size() > 1)
            result += ',';
        result += '"' + std::string(std::meta::identifier_of(member)) + "\":";
        if constexpr (std::is_same_v<F, bool> || (std::is_arithmetic_v<F> && !std::is_enum_v<F>))
            result += format(value);
        else
            result += '"' + format(value) + '"';  // the prototype's strings never need escaping
    }
    return result + "}";
}

}  // namespace dom_prototype::reflected

#endif
