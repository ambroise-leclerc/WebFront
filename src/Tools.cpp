#include <components/Terminal.hpp>
#include <components/ToolManager.hpp>
#include <WebFront.hpp>

// LCOV_EXCL_START - interactive example; the tool manager and its page are covered by the tests.
// Installs web tools from the npm registry into the working directory: the files go to
// .webfront/tools/ and the lock file to webfront-tools.lock.json. Development only: an installed
// tool can call every registered C++ function.
int main() {
    webfront::ToolsApp app;
    webfront::Terminal terminal{
        app,
        {.prompt = "\x1b[32mtools>\x1b[0m ", .banner = "WebFront tools. Type 'wf help'."}
    };
    webfront::ToolManager tools{app, terminal};
    app.show(terminal.page);
}
// LCOV_EXCL_STOP
