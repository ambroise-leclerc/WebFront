#include <frontend/WKWebView.hpp>

#include <iostream>

#include <dlfcn.h>

using namespace webfront::frontend::detail::wkwebview;

template <typename Function>
Function symbol(void* module, const char* name) {
    const auto address = dlsym(module, name);
    if (!address)
        throw std::runtime_error(std::string("Missing module symbol: ") + name);
    return reinterpret_cast<Function>(address);
}

void checkModule(const char* path) {
    auto module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!module)
        throw std::runtime_error(dlerror());
    const auto moduleDelegate = symbol<void* (*)()>(module, "webfrontWKDelegateClass")();
    const auto moduleView     = symbol<void* (*)()>(module, "webfrontWKViewClass")();
    if (moduleDelegate != delegateClass() || moduleView != viewClass())
        throw std::runtime_error("Runtime classes differ between executable and dylib");
    symbol<void (*)()>(module, "webfrontWKModuleSession")();
    // Registered methods retain code addresses from their defining image. Keep it loaded
    // until process exit, including when that image is the dylib rather than the executable.
}

int main(int argc, char** argv) {
    try {
        if (argc != 3)
            throw std::runtime_error("Expected a module path and registration order");
        initialize();
        const bool executableFirst = std::string_view(argv[2]) == "executable-first";
        if (executableFirst) {
            delegateClass();
            viewClass();
        }
        checkModule(argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
