# WebFront

A C++23 library with obvious purposes

[![Build Windows](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildWindows.yml/badge.svg)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildWindows.yml)

[![Build Ubuntu](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildLinux.yml/badge.svg)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildLinux.yml)

[![Build MacOS](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildMacOS.yml/badge.svg)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildMacOS.yml)

[![Ubuntu/Clang](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildLinuxClang.yml/badge.svg)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildLinuxClang.yml)



##Usage

```cpp
    WebFront webfront;
    webFront.onUIStarted([](UI ui) {
        ui.addScript("                                                          "
        "    var addText = function(text, num) {                                "
        "        let newDiv = document.createElement('div');                    "
        "        let newContent = document.createTextNode(text + ' of ' + num); "
        "        newDiv.appendChild(newContent);                                "
        "        document.body.appendChild(newDiv);                             "
        "    }                                                                  "
        );
        auto print = ui.jsFunction("addText");
        print("Hello World", 2022);
    });
```
