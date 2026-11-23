#include <webfront/testing/Bdd.hpp>
#include <webfront/system/SafePath.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using namespace webfront::bdd;

using namespace std;
using namespace webfront;

const Scenario pathsBrowser{"Paths from the browser stay inside their directory", [] {
    given("a relative path of plain components", [&] {
        then("it is accepted, component by component", [&] {
            require(fs::safeRelativePath("dist/assets/app.js") == filesystem::path("dist") / "assets" / "app.js");
            require(fs::safeRelativePath("ui/main.tsx") == filesystem::path("ui") / "main.tsx");
        });
    });
    given("unsafe paths", [&] {
        then("they are rejected", [&] {
            const vector<string> paths{"", "/etc/passwd", "../x", "a/../../x", "a/./b", "a//b", "a/", "..", "...", "C:/x", "c:x",
                                       "a\\..\\b", "a?.js", "a*.js", "a<b", "a>b", "a|b", "a\"b", "main..html", "a/b..c/d", string("a\0b", 3), string(4097, 'a')};
            for (const auto& path : paths) {
                auto scope = info(path);
                requireThrows<invalid_argument>([&] { fs::safeRelativePath(path); });
            }
        });
    });
}};
