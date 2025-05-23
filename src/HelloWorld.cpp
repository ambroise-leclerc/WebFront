#ifdef _WIN32
#include <windows.h>
#include <wrl/client.h> // For ComPtr
#include <wil/com.h>    // For WIL error handling utilities
#include <string>
#include <vector>
#include <WebView2EnvironmentOptions.h> // Required for CreateCoreWebView2EnvironmentWithOptions
#include <WebView2.h>                   // Required for ICoreWebView2 etc.

// Forward declarations
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

// Global variables for Win32 and WebView2
HWND g_hWnd = nullptr;
Microsoft::WRL::ComPtr<ICoreWebView2Controller> g_webView2Controller;
Microsoft::WRL::ComPtr<ICoreWebView2> g_webView2;

// Function to convert std::string to std::wstring
std::wstring widen(const std::string& narrow_string) {
    if (narrow_string.empty()) {
        return std::wstring();
    }
    int wide_char_count = MultiByteToWideChar(CP_UTF8, 0, narrow_string.c_str(), -1, nullptr, 0);
    if (wide_char_count == 0) {
        // Consider logging an error or throwing an exception
        return std::wstring();
    }
    std::vector<wchar_t> wide_string_buffer(wide_char_count);
    MultiByteToWideChar(CP_UTF8, 0, narrow_string.c_str(), -1, wide_string_buffer.data(), wide_char_count);
    return std::wstring(wide_string_buffer.data());
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        if (g_webView2Controller != nullptr) {
            RECT bounds;
            GetClientRect(hWnd, &bounds);
            g_webView2Controller->put_Bounds(bounds);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

#endif // _WIN32

#include <WebFront.hpp>
#include <system/BabelFS.hpp>
#include <system/IndexFS.hpp>
#include <system/NativeFS.hpp>
#include <system/ReactFS.hpp>

#include <filesystem>
#include <iostream>

/// Open the web UI in the system's default browser
auto openInDefaultBrowser(std::string_view port, std::string_view file) {
#ifdef _WIN32
    // For Windows, WebView2 will handle the UI, so we don't launch an external browser.
    // log::info("WebView2 will be used on Windows, openInDefaultBrowser is disabled."); // Placeholder for potential logging
    (void)port; // Mark as unused
    (void)file; // Mark as unused
    return 0; // Indicate success, as no action is needed.
#elif __linux__
    auto command = std::string("xdg-open ");
    auto url = std::string("http://localhost:").append(port) + std::string("/").append(file);
    return ::system(command.append(url).c_str());
#elif __APPLE__
    auto command = std::string("open ");
    auto url = std::string("http://localhost:").append(port) + std::string("/").append(file);
    return ::system(command.append(url).c_str());
#else
    // For other platforms, or if no platform macro is defined, do nothing or log a warning.
    // log::warn("openInDefaultBrowser not implemented for this platform.");
    (void)port; // Mark as unused
    (void)file; // Mark as unused
    return 1; // Indicate an issue or unsupported platform.
#endif
}

using namespace std;
using namespace webfront;

// Find the DocRoot path containing the given file (look for the filename in current directory,
// in temp directory then in sources directory
//
// @param filename
// @return DocRoot path
filesystem::path findDocRoot(string filename) {
    using namespace filesystem;

    log::info("Looking for {} in {}", filename, current_path().string());
    if (exists(current_path() / filename)) return current_path();

    log::info("  not found : Looking now in temp directory {} ", temp_directory_path().string());
    if (exists(temp_directory_path() / filename)) return temp_directory_path();

    path webfront, cp{current_path()};
    for (auto element = cp.begin(); element != cp.end(); ++element) {
        webfront = webfront / *element;
        if (*element == "WebFront") {
            webfront = webfront / "src";
            log::info("  not found : Looking now in source directory {}", webfront.string());
            if (exists(webfront / filename)) return webfront;
        }
    }

    throw runtime_error("Cannot find " + filename + " file");
}

int main(int argc, char** argv) { // Keep argc and argv as they might be used by WebFront or other logic
#ifdef _WIN32
    // COM Initialization
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        // Use cerr for early errors as log might not be set up
        std::cerr << "Failed to initialize COM library. Error code: " << hr << std::endl;
        return 1;
    }

    // --- WebView2 Runtime Detection START ---
    LPWSTR version_info_ptr = nullptr;
    HRESULT hr_version_check = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version_info_ptr);

    // Initialize WebFront logging early if needed for these messages, or use std::cout/cerr.
    // For now, we'll assume log:: is available or we'll use std::cout/cerr for this critical check.
    // The main WebFront logging (log::setLogLevel, log::addSinks) is set up later.
    // If log is not ready, these specific messages might need to use std::cout/cerr.
    // For the purpose of this step, we'll use log:: assuming it can buffer or is set up minimally.
    // A robust solution might initialize logging earlier or use MessageBox for critical errors.

    if (SUCCEEDED(hr_version_check) && version_info_ptr != nullptr) {
        std::wstring version_str(version_info_ptr);
        // The existing log::info uses fmt::format style, so %S is not standard.
        // Let's convert wstring to string for logging, or ensure logger supports wstring.
        // For simplicity with existing log::info, we'll attempt to log the wstring directly if supported,
        // or convert if there's an issue. A common safe approach is to convert to UTF-8 string.
        // For now, let's assume a simple conversion or direct wchar_t* support for logging.
        // This part might need adjustment based on the exact capabilities of `webfront::log`.
        // A quick conversion for logging:
        std::string version_std_str;
        if (version_info_ptr) {
            int len = WideCharToMultiByte(CP_UTF8, 0, version_info_ptr, -1, NULL, 0, NULL, NULL);
            if (len > 0) {
                version_std_str.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, version_info_ptr, -1, &version_std_str[0], len, NULL, NULL);
            }
        }
        log::info("WebView2 Runtime found. Version: {}", version_std_str);
        CoTaskMemFree(version_info_ptr);
    } else {
        // Log error (log::error might not be set up yet, consider MessageBox or cerr)
        std::wcerr << L"WebView2 Runtime is NOT installed or is incompatible. HRESULT: 0x" 
                   << std::hex << hr_version_check << std::endl;
        std::wcerr << L"Please install the WebView2 Runtime from Microsoft to use this application's graphical interface on Windows." << std::endl;
        // Display a message box as this is a critical failure for UI
        MessageBox(nullptr, 
                   L"WebView2 Runtime is not installed or is incompatible. "
                   L"Please download and install it from Microsoft's website to proceed. "
                   L"The application will now exit.",
                   L"WebView2 Runtime Error", 
                   MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1; // Exit application
    }
    // --- WebView2 Runtime Detection END ---

    // These variables are needed for URL construction and WebFront initialization
    auto httpPort = "9002";
    auto mainHtml = "react.html";
    
    // Initialize WebFront (logging, FS setup, etc.)
    // This needs to be done before WebView2 might need the URL or interact with WebFront.
    using HelloFS = fs::Multi<fs::NativeDebugFS, fs::IndexFS, fs::ReactFS, fs::BabelFS>;
    using WebFrontDbg = BasicWF<NetProvider, HelloFS>;
    filesystem::path docRoot; // Define docRoot before try-catch
    try {
        docRoot = findDocRoot(mainHtml);
    } catch (const std::runtime_error& e) {
        std::cerr << "Error finding doc root: " << e.what() << std::endl;
        CoUninitialize();
        return 1;
    }

    std::cout << "WebFront launched from " << filesystem::current_path().string() << "\n"; // Use cout for general info
    log::setLogLevel(log::Debug); // WebFront logging
    log::addSinks(log::clogSink);
    WebFrontDbg webFront(httpPort, docRoot); // webFront instance

    // Setup C++/JS interop (this needs to be available when WebView2 loads the page)
    webFront.cppFunction<void, std::string>("print", [](std::string text) { std::cout << text << '\n'; });
    webFront.onUIStarted([](WebFrontDbg::UI ui) {
        ui.addScript("var addText = function(text, num) {                 \n"
                     "  let print = webFront.cppFunction('print');        \n"
                     "  print(text + ' of ' + num);                       \n"
                     "}                                                   \n"
                     "                                                    \n"
                     "var testFunc = function(text) {                     \n"
                     "  let bigText = 'bigText : ' + text + text + ' - '; \n"
                     "  bigText += bigText + bigText;                     \n"
                     "  let cppTest = webFront.cppFunction('cppTest');    \n"
                     "  cppTest(text, bigText, bigText.length);           \n"
                     "}                                                   \n");
        auto print = ui.jsFunction("addText");
        print("Hello World", 2022);
        ui.jsFunction("testFunc")("Texte de test suffisament long pour changer de format");
    });
    // Note: webFront.run() will not be called here for Win32.
    // The HTTP server aspect of WebFront needs to be started,
    // potentially on a separate thread, or its IO integrated into the message loop.
    // For now, we assume WebFront's constructor or another method starts the necessary server parts
    // if they are to run independently. This will be addressed in a future step.
    // The current `webFront` object setup ensures that JS can call C++ functions.

    // Register window class
    WNDCLASSEX wcex = {0}; // Initialize with {0}
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = GetModuleHandle(nullptr); // Use GetModuleHandle(nullptr) for hInstance
    wcex.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = nullptr;
    wcex.lpszClassName = L"WebFrontWindowClass";
    wcex.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassEx(&wcex)) {
        log::error("Failed to register window class. Error code: {}", GetLastError());
        CoUninitialize();
        return 1;
    }

    // Create main window
    g_hWnd = CreateWindowEx(
        0, L"WebFrontWindowClass", L"WebFront Application", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, // Default window size
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

    if (!g_hWnd) {
        log::error("Failed to create window. Error code: {}", GetLastError());
        CoUninitialize();
        return 1;
    }

    ShowWindow(g_hWnd, SW_SHOWDEFAULT); // Use SW_SHOWDEFAULT from winuser.h
    UpdateWindow(g_hWnd);

    // WebView2 Initialization
    hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [=](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result)) {
                    log::error("Failed to create WebView2 environment. Error code: {}", result);
                    return result;
                }
                log::info("WebView2 Environment created successfully.");

                result = env->CreateCoreWebView2Controller(g_hWnd,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [=](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result)) {
                                log::error("Failed to create WebView2 controller. Error code: {}", result);
                                return result;
                            }
                            log::info("WebView2 Controller created successfully.");

                            g_webView2Controller = controller;
                            // QueryInterface for ICoreWebView2 is preferred over get_CoreWebView2 for robustness
                            // result = g_webView2Controller->get_CoreWebView2(&g_webView2);
                            result = g_webView2Controller.As(&g_webView2); // Use ComPtr's As() for QueryInterface
                            if (FAILED(result)) {
                                log::error("Failed to get CoreWebView2 from controller. Error code: {}", result);
                                return result;
                            }

                            RECT bounds;
                            GetClientRect(g_hWnd, &bounds);
                            g_webView2Controller->put_Bounds(bounds);
                            g_webView2Controller->put_IsVisible(TRUE);
                            
                            // Optional: Add NavigationCompleted event handler
                            EventRegistrationToken token;
                            g_webView2->add_NavigationCompleted(Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                [](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                    BOOL success;
                                    args->get_IsSuccess(&success);
                                    if (!success) {
                                        COREWEBVIEW2_WEB_ERROR_STATUS webErrorStatus;
                                        args->get_WebErrorStatus(&webErrorStatus);
                                        log::error("WebView2 navigation failed with status: {}", static_cast<int>(webErrorStatus));
                                    } else {
                                        log::info("WebView2 navigation completed successfully.");
                                    }
                                    return S_OK; // Always return S_OK from event handlers
                                }).Get(), &token);


                            std::string url_str = std::string("http://localhost:") + httpPort + "/" + mainHtml;
                            log::info("Navigating WebView2 to: {}", url_str);
                            result = g_webView2->Navigate(widen(url_str).c_str());
                            if (FAILED(result)) {
                                log::error("WebView2 navigation command failed. Error code: {}", result);
                            }
                            
                            return S_OK;
                        }).Get());
                if (FAILED(result)) {
                    log::error("Failed to initiate WebView2 controller creation. Error code: {}", result);
                }
                return result; // Return result from outer callback as well
            }).Get());

    if (FAILED(hr)) {
        log::error("Failed to initiate WebView2 environment creation. Error code: {}", hr);
        CoUninitialize();
        return 1;
    }

    // Modified message loop to integrate webFront.runOne()
    MSG msg = {0};
    bool appIsRunning = true;
    while (appIsRunning) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                appIsRunning = false; // Signal loop termination
            } else {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        } else {
            // No window messages, so process WebFront events.
            webFront.runOne(); 
        }
    }

    CoUninitialize();
    // When WM_QUIT is received, msg.wParam contains the exit code.
    // If the loop exits due to appIsRunning being set false by other means (not shown here),
    // msg might not be WM_QUIT. Defaulting to 0 or a specific success code is common.
    // If PostQuitMessage was called, msg.wParam from WM_QUIT is the value to return.
    return (msg.message == WM_QUIT) ? (int)msg.wParam : 0;

#else // For non-Windows platforms (original main logic)

    // Original main logic for Linux, macOS, etc.
    using HelloFS = fs::Multi<fs::NativeDebugFS, fs::IndexFS, fs::ReactFS, fs::BabelFS>;
    using WebFrontDbg = BasicWF<NetProvider, HelloFS>;


    auto httpPort = "9002";
    auto mainHtml = "react.html";
    auto docRoot = findDocRoot(mainHtml);

    cout << "WebFront launched from " << filesystem::current_path().string() << "\n";
    log::setLogLevel(log::Debug);
    log::addSinks(log::clogSink);
    WebFrontDbg webFront(httpPort, docRoot);

    webFront.cppFunction<void, std::string>("print", [](std::string text) { std::cout << text << '\n'; });
    
    webFront.onUIStarted([](WebFrontDbg::UI ui) {
        ui.addScript("var addText = function(text, num) {                 \n"
                     "  let print = webFront.cppFunction('print');        \n"
                     "  print(text + ' of ' + num);                       \n"
                     "}                                                   \n"
                     "                                                    \n"
                     "var testFunc = function(text) {                     \n"
                     "  let bigText = 'bigText : ' + text + text + ' - '; \n"
                     "  bigText += bigText + bigText;                     \n"
                     "  let cppTest = webFront.cppFunction('cppTest');    \n"
                     "  cppTest(text, bigText, bigText.length);           \n"
                     "}                                                   \n");
        auto print = ui.jsFunction("addText");
        print("Hello World", 2022);
        ui.jsFunction("testFunc")("Texte de test suffisament long pour changer de format");
    });

    openInDefaultBrowser(httpPort, mainHtml); // This is already disabled for Windows
    webFront.run();
    return 0; 
#endif // _WIN32
}