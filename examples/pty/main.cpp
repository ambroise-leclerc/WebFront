#include <webfront/PtyPage.hpp>
#include <webfront/WebFront.hpp>
#ifdef _WIN32
    #include <webfront/system/WindowsCompat.hpp>
#endif

#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: WebFrontPtyExample <wf build output directory>\n";
        return 1;
    }

#ifdef _WIN32
    // This executable opts its shell children into the Windows console's Ctrl+C processing.
    ::SetConsoleCtrlHandler(nullptr, FALSE);
#endif
    webfront::PtyOptions shell{
        .argv =
            {
#ifdef _WIN32
                   "cmd.exe", "/d"
#else
                "/bin/sh",
                "-i"
#endif
            },
        .directory   = std::filesystem::current_path(),
        .environment = {},
        .size        = {}
    };
    if (const char* path = std::getenv("PATH"))
        shell.environment.emplace("PATH", path);
    webfront::WebFront app({.documentRoot = argv[1], .contentPolicy = {.allowMissingOrigin = false}, .allowPtyPages = true});
    webfront::registerPtyPage(app, std::move(shell));
    app.show();
}
