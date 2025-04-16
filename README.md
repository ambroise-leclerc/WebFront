# WebFront

A C++23 header-only library with the aim of providing rich cross-platform UI (Typescript based) to C++ applications with a non-invasive API.

WebFront implements the websocket protocol over an embedded Web server and provides CppToJs and JsToCpp cross functions calls with native types conversions.

[![Windows MSVC](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml/badge.svg?branch=master&os=windows-latest&compiler=msvc)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![Ubuntu GCC](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml/badge.svg?branch=master&os=ubuntu-latest&compiler=gcc)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![Ubuntu Clang](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml/badge.svg?branch=master&os=ubuntu-latest&compiler=clang)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![MacOS Clang](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml/badge.svg?branch=master&os=macos-latest&compiler=clang)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![codecov](https://codecov.io/github/ambroise-leclerc/WebFront/branch/master/graph/badge.svg?token=ODE6O36XIV)](https://codecov.io/github/ambroise-leclerc/WebFront)

[![CodeScene Code Health](https://codescene.io/projects/29377/status-badges/code-health)](https://codescene.io/projects/29377)
[![CodeScene System Mastery](https://codescene.io/projects/29377/status-badges/system-mastery)](https://codescene.io/projects/29377)
[![SonarCloud](https://sonarcloud.io/api/project_badges/measure?project=ambroise-leclerc_WebFront&metric=alert_status)](https://sonarcloud.io/dashboard?id=ambroise-leclerc_WebFront)


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
        print("Hello World", 2023);
    });

```
