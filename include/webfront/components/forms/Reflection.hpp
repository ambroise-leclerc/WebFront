/// @brief Optional C++26 layer (ADR-018, #383): describe<T>() produces by static reflection (P2996, annotations P3394,
/// expansion statements P1306) the same descriptors that a C++23 application writes by hand with field() and choice().
/// Members are annotated with [[=forms::Label{"..."}]] and [[=forms::Range{min, max}]]. Without reflection
/// (`__cpp_impl_reflection`, GCC 16 with -std=c++26 -freflection) this header declares nothing: the core stays C++23.
#pragma once
#include "Description.hpp"

#if defined(__cpp_impl_reflection) && defined(__cpp_expansion_statements)

    #include <concepts>
    #include <cstddef>
    #include <meta>
    #include <optional>
    #include <string>
    #include <string_view>
    #include <type_traits>
    #include <utility>
    #include <vector>

namespace webfront::forms {

/// The label of a member. A string literal is not a structural value: the text is copied into static storage.
struct Label {
    const char* text;
    consteval Label(const char* value) : text(std::define_static_string(std::string_view(value))) { }
};

/// The inclusive bounds of a numeric member.
struct Range {
    double min;
    double max;
};

namespace detail {

template <typename Annotation, std::meta::info member>
consteval std::size_t annotationCount() {
    return std::meta::annotations_of_with_type(member, ^^Annotation).size();
}

// The annotation of a member, which describe() requires to be unique.
template <typename Annotation, std::meta::info member>
consteval std::optional<Annotation> annotation() {
    for (auto value : std::meta::annotations_of_with_type(member, ^^Annotation))
        return std::meta::extract<Annotation>(value);
    return std::nullopt;
}

template <typename E>
std::vector<std::pair<std::string, E>> enumerators() {
    std::vector<std::pair<std::string, E>> result;
    template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^E)))
        result.emplace_back(std::string(std::meta::identifier_of(e)), [:e:]);
    return result;
}

}  // namespace detail

/** @brief The description of the public non-static data members of T, in declaration order: the type and member names,
 * the Label annotations (or the names) and the Range annotations. Enumerations become choices named after their
 * enumerators.
 */
template <typename T>
Description<T> describe() {
    static_assert(std::is_class_v<T> && std::meta::has_identifier(^^T), "forms::describe needs a named class type");
    Description<T> result{.name = std::string(std::meta::identifier_of(^^T)), .fields = {}};
    template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
        // A repeated annotation would silently lose all but its first value.
        static_assert(detail::annotationCount<Label, member>() <= 1, "forms::Label is repeated on a member");
        static_assert(detail::annotationCount<Range, member>() <= 1, "forms::Range is repeated on a member");
        constexpr auto label = detail::annotation<Label, member>();
        constexpr auto range = detail::annotation<Range, member>();
        std::string    name(std::meta::identifier_of(member));
        std::string    text = label ? std::string(label->text) : name;
        // GCC 16.1 rejects `using F = [:type_of(member):]` followed by a splice in a discarded branch: name the type
        // through decltype of an access instead.
        using F = std::remove_cvref_t<decltype(std::declval<T&>().[:member:])>;
        static_assert(!range.has_value() || (std::is_arithmetic_v<F> && !std::same_as<F, bool>), "forms::Range applies to numeric members only");
        if constexpr (std::is_enum_v<F>) {
            result.fields.push_back(choice(std::move(name), &[:member:], detail::enumerators<F>(), std::move(text)));
        } else {
            std::optional<Bounds> bounds;
            if constexpr (range.has_value())
                bounds = Bounds{range->min, range->max};
            result.fields.push_back(field(std::move(name), &[:member:], std::move(text), bounds));
        }
    }
    return result;
}

}  // namespace webfront::forms

#endif
