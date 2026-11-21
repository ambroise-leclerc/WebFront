#pragma once

#include <webfront/WebFront.hpp>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace webfront::demo {

/// Handles `--port <n>` and `--port=<n>` of an example: removes the option from `argv` and makes it the default port of the
/// applications that follow (the environment variable WEBFRONT_PORT, which the example also honours without it).
/// Prints the reason and returns false when the value is not a port number or is missing.
[[nodiscard]] inline bool applyPortOption(int& argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        std::string_view       value;
        int                    used = 1;
        if (argument == "--port") {
            if (i + 1 >= argc) {
                std::cerr << "--port needs a port number\n";
                return false;
            }
            value = argv[i + 1];
            used  = 2;
        } else if (argument.starts_with("--port=")) {
            value = argument.substr(7);
        } else {
            continue;
        }
        try {
            const std::string port = validatedPort(value, "--port");
#ifdef _WIN32
            _putenv_s("WEBFRONT_PORT", port.c_str());
#else
            setenv("WEBFRONT_PORT", port.c_str(), 1);
#endif
        } catch (const std::invalid_argument& error) {
            std::cerr << error.what() << '\n';
            return false;
        }
        for (int j = i; j + used <= argc; ++j)
            argv[j] = argv[j + used];  // moves the trailing null pointer too
        argc -= used;
        --i;
    }
    return true;
}

}  // namespace webfront::demo
