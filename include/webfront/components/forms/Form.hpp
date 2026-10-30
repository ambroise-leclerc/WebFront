/// @brief Optional forms and inspectors built in a DOM document (ADR-018, #383) from the descriptors of Description.hpp.
/// They only read descriptors, whether written by hand in C++23 or produced by the C++26 reflection layer.
#pragma once
#include "../../dom/Document.hpp"
#include "Description.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace webfront::forms {

namespace detail {

template <typename T>
struct FormState {
    std::shared_ptr<T>                    object;
    Description<T>                        description;
    std::vector<dom::Node>                controls;
    std::vector<bool>                     invalid;
    std::function<void(std::string_view)> changed;

    // aria-invalid marks an input whose value was refused; the member keeps its previous value.
    void mark(std::size_t index, bool refused) {
        if (invalid[index] == refused)
            return;
        invalid[index] = refused;
        if (refused)
            controls[index].attribute("aria-invalid", "true");
        else
            controls[index].unsetAttribute("aria-invalid");
    }
    void input(std::size_t index, std::string_view value) {
        const auto& field    = description.fields[index];
        const bool  accepted = field.write(*object, value);
        mark(index, !accepted);
        // A copy: the callback may replace itself through onChange() while it runs.
        if (accepted && changed) {
            const auto callback = changed;
            callback(field.name);
        }
    }
};

template <typename T>
struct InspectorState {
    std::shared_ptr<const T> object;
    Description<T>           description;
    std::vector<dom::Node>   values;
    std::vector<std::string> shown;
};

inline std::size_t indexOf(const auto& fields, std::string_view name) {
    for (std::size_t i = 0; i < fields.size(); ++i)
        if (fields[i].name == name)
            return i;
    throw std::out_of_range("No form field named " + std::string(name));
}

}  // namespace detail

/** @brief A form built in a document: one labelled input per field. A valid browser value writes the member at once;
 * a refused one leaves the member unchanged and marks the input `aria-invalid`. Copies share the same form. All methods
 * follow the document's server-thread and lifetime rules (docs/DOM.md).
 */
template <typename T>
class Form {
    std::shared_ptr<detail::FormState<T>> state;
    dom::Node                             root;

public:
    /// Built by forms::form().
    Form(std::shared_ptr<detail::FormState<T>> s, dom::Node r) : state(std::move(s)), root(r) { }
    /// The `form` element, to remove or move the whole form.
    dom::Node node() const {
        return root;
    }
    /// The input or select of a field; throws std::out_of_range for an unknown name.
    dom::Node control(std::string_view name) const {
        return state->controls[detail::indexOf(state->description.fields, name)];
    }
    /// Called with the field name after the browser has changed a member.
    void onChange(std::function<void(std::string_view)> callback) const {
        state->changed = std::move(callback);
    }
    /// Shows the object's current values, after C++ has changed them, and clears the invalid marks.
    void refresh() const {
        for (std::size_t i = 0; i < state->controls.size(); ++i) {
            const auto& field = state->description.fields[i];
            const auto  value = field.read(*state->object);
            const auto& input = state->controls[i];
            if (field.kind == Kind::boolean)
                input.property("checked", value);
            else if (field.kind == Kind::choice) {
                std::ptrdiff_t selected = -1;
                for (std::size_t c = 0; c < field.choices.size(); ++c)
                    if (field.choices[c] == value)
                        selected = static_cast<std::ptrdiff_t>(c);
                input.property("selectedIndex", std::to_string(selected));
            } else
                input.property("value", value);
            state->mark(i, false);
        }
    }
};

/** @brief Builds a form for object in parent and returns it. The form shares ownership of the object, which its event
 * callbacks write on the server context. Throws std::invalid_argument for an invalid description (see validate()).
 */
template <typename UI, typename T>
Form<T> form(const dom::Document<UI>& doc, dom::Node parent, std::shared_ptr<T> object, Description<T> description) {
    validate(description);
    if (!object)
        throw std::invalid_argument("A form needs an object");
    auto state = std::make_shared<detail::FormState<T>>(
        detail::FormState<T>{.object = std::move(object), .description = std::move(description), .controls = {}, .invalid = {}, .changed = {}});
    auto root = doc.element("form");
    root.attribute("class", "wf-form");
    // Enter in a text input submits the form: the page must not navigate.
    root.listen("submit", [](std::string) { }, {.preventDefault = true, .stopPropagation = false});
    for (std::size_t i = 0; i < state->description.fields.size(); ++i) {
        const auto& field   = state->description.fields[i];
        const auto  value   = field.read(*state->object);
        auto        row     = doc.element("label");
        auto        caption = doc.element("span");
        row.attribute("class", "wf-field");
        caption.append(doc.text(field.label));
        row.append(caption);
        dom::Node control;
        if (field.kind == Kind::choice) {
            control = doc.element("select");
            for (const auto& choice : field.choices) {
                auto option = doc.element("option");
                option.attribute("value", choice);
                if (choice == value)
                    option.attribute("selected", "");
                option.append(doc.text(choice));
                control.append(option);
            }
            // A value missing from the choices selects nothing, as refresh() does, rather than the first option.
            if (std::ranges::find(field.choices, value) == field.choices.end())
                control.property("selectedIndex", "-1");
        } else {
            control = doc.element("input");
            if (field.kind == Kind::boolean) {
                control.attribute("type", "checkbox");
                if (value == "true")
                    control.attribute("checked", "");
            } else {
                const bool numeric = field.kind == Kind::integer || field.kind == Kind::number;
                control.attribute("type", numeric ? "number" : "text");
                control.attribute("value", value);
                if (numeric)
                    control.attribute("step", field.kind == Kind::integer ? "1" : "any");
                if (field.range) {
                    control.attribute("min", detail::formatNumber(field.range->min));
                    control.attribute("max", detail::formatNumber(field.range->max));
                }
            }
        }
        control.attribute("name", field.name);
        const bool changeEvent = field.kind == Kind::boolean || field.kind == Kind::choice;
        control.listen(changeEvent ? "change" : "input", [state, i](std::string text) {
            state->input(i, text);
        });
        row.append(control);
        root.append(row);
        state->controls.push_back(control);
        state->invalid.push_back(false);
    }
    parent.append(root);
    return Form<T>{std::move(state), root};
}

/** @brief A read-only table of the fields of an object, with labels; refresh() updates the values that have changed
 * since the last refresh, as text nodes. Copies share the same inspector.
 */
template <typename T>
class Inspector {
    std::shared_ptr<detail::InspectorState<T>> state;
    dom::Node                                  root;

public:
    /// Built by forms::inspector().
    Inspector(std::shared_ptr<detail::InspectorState<T>> s, dom::Node r) : state(std::move(s)), root(r) { }
    /// The `table` element.
    dom::Node node() const {
        return root;
    }
    /// The text node that shows a field; throws std::out_of_range for an unknown name.
    dom::Node value(std::string_view name) const {
        return state->values[detail::indexOf(state->description.fields, name)];
    }
    void refresh() const {
        for (std::size_t i = 0; i < state->values.size(); ++i) {
            auto text = state->description.fields[i].read(*state->object);
            if (text != state->shown[i]) {
                state->values[i].setText(text);
                state->shown[i] = std::move(text);
            }
        }
    }
};

/// Builds an inspector of object in parent. Throws std::invalid_argument for an invalid description.
template <typename UI, typename T>
Inspector<T> inspector(const dom::Document<UI>& doc, dom::Node parent, std::type_identity_t<std::shared_ptr<const T>> object, Description<T> description) {
    validate(description);
    if (!object)
        throw std::invalid_argument("An inspector needs an object");
    auto state = std::make_shared<detail::InspectorState<T>>(
        detail::InspectorState<T>{.object = std::move(object), .description = std::move(description), .values = {}, .shown = {}});
    auto root = doc.element("table");
    auto body = doc.element("tbody");
    root.attribute("class", "wf-inspector");
    root.append(body);
    for (const auto& field : state->description.fields) {
        auto row   = doc.element("tr");
        auto label = doc.element("th");
        auto cell  = doc.element("td");
        label.append(doc.text(field.label));
        auto shown = field.read(*state->object);
        auto value = doc.text(shown);
        cell.append(value);
        row.append(label);
        row.append(cell);
        body.append(row);
        state->values.push_back(value);
        state->shown.push_back(std::move(shown));
    }
    parent.append(root);
    return Inspector<T>{std::move(state), root};
}

}  // namespace webfront::forms
