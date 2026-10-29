// The settings type that the forms tests describe by hand in C++23 (#383). The C++26 reflection test annotates the same
// members and must produce the same declaration and the same form; the developer journey writes this declaration into
// its project and type-checks a module that uses it with wf check.
#pragma once
#include <webfront/components/forms/Description.hpp>

#include <string>
#include <string_view>

namespace forms_fixture {

enum class Pattern { glider, pulsar, random };

struct Settings {
    int         speed{5};
    double      density{0.25};
    bool        wrap{true};
    Pattern     pattern{Pattern::glider};
    std::string title{"Life"};
};

inline webfront::forms::Description<Settings> settingsDescription() {
    using namespace webfront::forms;
    return {
        .name   = "Settings",
        .fields = {field("speed", &Settings::speed, "Speed", Bounds{1, 10}),
                   field("density", &Settings::density, "Initial density", Bounds{0, 1}),
                   field("wrap", &Settings::wrap, "Wrap around edges"),
                   choice("pattern", &Settings::pattern, {{"glider", Pattern::glider}, {"pulsar", Pattern::pulsar}, {"random", Pattern::random}}),
                   field("title", &Settings::title)}
    };
}

inline constexpr std::string_view settingsDeclaration = R"(export interface Settings {
    /** Speed (1 to 10) */
    readonly speed: number;
    /** Initial density (0 to 1) */
    readonly density: number;
    /** Wrap around edges */
    readonly wrap: boolean;
    readonly pattern: "glider" | "pulsar" | "random";
    readonly title: string;
}
)";

}  // namespace forms_fixture
