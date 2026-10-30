# Forms from type descriptions

The optional headers under `include/webfront/components/forms/` build settings panels, forms and
inspectors in a [DOM document](DOM.md) from a description of a C++ type, and generate the TypeScript
declaration of that type for `wf check` ([ADR-018](adr/ADR-018-acces-au-dom.md), #383).

- `Description.hpp` (C++23): descriptors, their validation and `typescriptDeclaration()`.
- `Form.hpp` (C++23): `forms::form()` and `forms::inspector()`, which only read descriptors.
- `Reflection.hpp` (optional C++26): `forms::describe<T>()` produces the same descriptors by static
  reflection. Without `__cpp_impl_reflection` the header declares nothing; the library stays C++23.

## Describing a type in C++23

A `forms::Description<T>` is the type's TypeScript name and its fields in display order. Each
`forms::Field<T>` holds a name, a label, a kind (`boolean`, `integer`, `number`, `text`, `choice`),
optional inclusive bounds, the names of the choices, and two accessors: `read` formats the member as
the browser shows it, `write` parses a browser value and returns `false`, leaving the member
unchanged, when the value does not parse or is outside the bounds or choices.

```cpp
#include <webfront/components/forms/Form.hpp>

enum class Pattern { glider, pulsar, random };
struct Settings {
    int         speed{5};
    double      density{0.25};
    bool        wrap{true};
    Pattern     pattern{Pattern::glider};
    std::string title{"Life"};
};

webfront::forms::Description<Settings> describeSettings() {
    using namespace webfront::forms;
    return {.name   = "Settings",
            .fields = {field("speed", &Settings::speed, "Speed", Bounds{1, 10}),
                       field("density", &Settings::density, "Initial density", Bounds{0, 1}),
                       field("wrap", &Settings::wrap, "Wrap around edges"),
                       choice("pattern", &Settings::pattern,
                              {{"glider", Pattern::glider}, {"pulsar", Pattern::pulsar}, {"random", Pattern::random}}),
                       field("title", &Settings::title)}};
}
```

`field()` accepts `bool`, arithmetic and `std::string` members; enumerations and other values with a
fixed set use `choice()`. The label defaults to the name. Bounds apply to numbers only. A description
can also be written field by field with custom accessors, for computed or nested values.

Floating-point numbers are formatted by `std::to_chars` as the shortest text that reads back as
the same value (`0.25`, `1`), independent of the locale, never with `std::to_string`, whose C++26
output differs from C++23 (P2587). Integers parse with `std::from_chars`; an integer input may also
report `5.0` or `1e1`, which is accepted when the value is whole and exact. Any other value with
trailing characters, out of range for the member type or outside the bounds is refused. Floating-point
values must be finite and must not underflow: a subnormal result, or a zero from non-zero digits
(`1e-400`), is refused with every standard library. The bounds of a `float` member are compared in
`float`, so the bound that the form shows (`0.1`) is itself accepted.

`forms::validate(description)` throws `std::invalid_argument` unless the type and field names are
identifiers (they become TypeScript members and input names) and unique, the type name is not a
word that TypeScript refuses for an interface, every field has both accessors, bounds are finite,
ordered, only on numbers and whole on integers, and choices are present, non-empty and
unique exactly for choice fields. The generators call it first and change nothing when it throws.
When an accessor or the document throws later, while a form or an inspector is built, the elements
already created are removed with their listeners and their share of the object, and the exception
propagates: the browser receives them followed by their removal.

## Forms

```cpp
app.onUIStarted([](auto ui) {
    auto doc      = ui.document();
    auto settings = std::make_shared<Settings>();
    auto form     = webfront::forms::form(doc, doc.island(), settings, describeSettings());
    form.onChange([](std::string_view field) { /* settings->[field] changed */ });
});
```

The form is one batch: a `form` element with a `label` per field, holding a `span` with the label
and the control. Numbers are `<input type="number">` with `min`, `max` and `step` (`1` for
integers, `any` otherwise), booleans are checkboxes, text is `<input type="text">` and choices are a
`select` whose options are the choice names. Numbers and text listen to `input`, checkboxes and
choices to `change`; `submit` is declared with `preventDefault`, so Enter never navigates.

A valid browser value writes the member at once, on the server context, then calls the `onChange`
callback with the field name. A refused value keeps the member and sets `aria-invalid="true"` on the
control; the next valid value removes it. The controls show the values through the `value`,
`checked` and `selectedIndex` properties, when the form is built and on `form.refresh()`, which
C++ calls after changing the object and which also clears the marks. A choice value missing from
the choices selects nothing (`selectedIndex` `-1`) rather than the first option.

The form, and the listeners of its controls, share ownership of the object: removing the form
element (`form.node().remove()`) or closing the document releases them. `form.control(name)` returns
the control of a field, for example to disable it; an unknown name throws `std::out_of_range`. All
calls follow the document's server-thread and lifetime rules ([DOM.md](DOM.md)), and only tags,
attributes and events of the interpreter's default allowlist are used. The class names `wf-form`,
`wf-field` and `wf-inspector` are left to the page's style sheet.

## Inspectors

`forms::inspector(doc, parent, object, description)` builds a read-only `table` with one row per
field: a `th` with the label and a `td` with the value as a text node. `refresh()` sends `setText`
for the values that changed since the previous refresh, nothing otherwise. The inspector shares
ownership of a `std::shared_ptr<const T>`.

## TypeScript declarations for `wf check`

`forms::typescriptDeclaration(description)` returns an interface whose members are `readonly`;
numbers are `number`, choices the union of their quoted names, and labels and bounds become
documentation comments (a `*/` in a label is escaped):

```ts
export interface Settings {
    /** Speed (1 to 10) */
    readonly speed: number;
    ...
    readonly pattern: "glider" | "pulsar" | "random";
    readonly title: string;
}
```

Write it to a `.d.ts` file under the directories that the project's `tsconfig.json` includes, for
example from a build step or an application [command](DEVELOPER_ENVIRONMENT.md), and import it with
`import type`. `wf check` then verifies TypeScript code against the C++ type. The developer journey
test (`JourneyDeclaration`, `JourneyCheck`) writes this declaration into its project and checks a
module that uses it, including `@ts-expect-error` lines for an unknown choice, a wrong type, an
extra member, a missing member and a write to a read-only member. The declaration describes values;
the bridge still transports only the types of [ADR-004](adr/ADR-004-appels-asynchrones-et-erreurs.md).

## Optional C++26 reflection

With a compiler that implements static reflection (today GCC 16 with `-std=c++26 -freflection`),
`Reflection.hpp` replaces the hand-written description by annotations:

```cpp
#include <webfront/components/forms/Reflection.hpp>
using webfront::forms::Label;
using webfront::forms::Range;

struct Settings {
    [[= Label{"Speed"}, = Range{1, 10}]] int        speed{5};
    [[= Label{"Initial density"}, = Range{0, 1}]] double density{0.25};
    [[= Label{"Wrap around edges"}]] bool           wrap{true};
    Pattern                                         pattern{Pattern::glider};
    std::string                                     title{"Life"};
};

auto form = webfront::forms::form(doc, doc.island(), settings, webfront::forms::describe<Settings>());
```

`describe<T>()` lists the public non-static data members of a named class in declaration order.
Members of base classes are not listed, and a bit-field does not compile: describe such a type by
hand with `field()`.
The type and member names come from their identifiers, labels from `Label` (or the name), bounds
from `Range`, and an enumeration becomes a choice named after its enumerators. A `Range` on a
member that is not a number (a `bool`, a string, an enumeration) and a repeated `Label` or `Range`
do not compile; the `C++26.FormsReflectionRejects` tests check each message. The
result is an ordinary `Description<T>`: forms, inspectors and declarations do not depend on how it
was produced. The `FormsReflectionTests` executable checks that the reflected description, its
declaration and its form batch equal the hand-written ones; it links a Catch2 compiled with the
same C++26 flags, so that no executable mixes objects of both standards. CMake builds it only when the compiler
supports reflection; the Linux CI job sets `-DWEBFRONT_REQUIRE_REFLECTION_TESTS=ON`, so that it
cannot be skipped there.

GCC 16.1 pitfalls handled by the layer: a string literal is not a structural value, so `Label`
copies its text with `std::define_static_string`; a `using F = [:type_of(m):]` alias followed by a
splice in a discarded `if constexpr` branch is rejected, so the member type is taken from
`decltype` of an access. Clang has no released support and MSVC none announced: applications that
must build there describe their types by hand.
