# WebFront

A C++23 header-only library with the aim of providing rich cross-platform UI (Typescript based) to C++ applications with a non-invasive API.

WebFront implements websocket protocol over an embedded Web server and provides C++_to_Typescript and Typescript_to_C++ cross functions calls with native types conversions.

[![Build Windows](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildWindows.yml/badge.svg)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildWindows.yml)
[![Build Ubuntu](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildLinux.yml/badge.svg)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildLinux.yml)
[![Ubuntu/Clang](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildLinuxClang.yml/badge.svg)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildLinuxClang.yml)
[![Build MacOS](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildMacOS.yml/badge.svg)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildMacOS.yml)



## Getting started

#### Hello World
```cpp
    WebFront webfront;
    webFront.cppFunction<void, std::string>("print", [](std::string text) {
        std::cout << text << '\n';
    });

    webFront.onUIStarted([](UI ui) {
        ui.addScript("var addText = function(text, num) {                 "
                     "  let print = webFront.cppFunction('print');        "
                     "  print(text + ' of ' + num);                       "
                     "}                                                   ");
        auto print = ui.jsFunction("addText");
        print("Hello World", 2022);
    });

```
