# <img src="docs/images/webfront-icon.svg" width="24" height="24" alt=""> WebFront

[![CI](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml/badge.svg?branch=develop)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![Windows · MSVC](https://img.shields.io/badge/CI-Windows%20%C2%B7%20MSVC-0078D4)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![macOS · Apple Clang](https://img.shields.io/badge/CI-macOS%20%C2%B7%20Apple%20Clang-555555)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![Linux · GCC](https://img.shields.io/badge/CI-Linux%20%C2%B7%20GCC-E95420)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![Linux · aarch64](https://img.shields.io/badge/CI-Linux%20%C2%B7%20aarch64%20%C2%B7%20GCC%2013-E95420)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![Jetson Orin](https://img.shields.io/badge/CI-Jetson%20Orin-76B900)](docs/JETSON.md)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C)](CMakeLists.txt)
[![Header-only](https://img.shields.io/badge/library-header--only-2EA043)](include/)

[English](README.md) · [Français](README.fr.md) · [简体中文](README.zh-CN.md)

WebFront 是一个 C++ UI 框架，用来构建现代、响应式的界面。它是纯头文件的 C++23 库：应用逻辑留在 C++ 中，界面使用 TypeScript、React 和兼容浏览器的第三方包构建，完成后的应用可以打包成一个可执行文件。

调用 `app.bootstrap("developer")`，应用就能打开自己的开发环境。在应用窗口中，`wf` 可以编译和预览界面、检查类型，并运行 Jasmine 测试。你可以在终端里操作，也可以让编程智能体通过 MCP 执行同样的流程。无需安装 Node.js 或 npm。

界面准备就绪后，将资源嵌入可执行文件。这样就能得到一个紧凑、自包含的应用，可在原生窗口中离线运行，也可在嵌入式系统和无图形会话的系统上通过 Web 浏览器离线使用。

[![生命游戏演示：在应用窗口里创建项目、安装依赖并预览 React 界面](docs/images/life-bootstrap.gif)](#从-c-模拟到可分发的生命游戏)

动画展示 C++ 应用和生命游戏示例。为了便于阅读，终端输出和等待时间有所压缩，预览画面有所放大。[查看下方教程](#从-c-模拟到可分发的生命游戏)。

- **让编程智能体参与开发。** 智能体可通过 MCP 创建界面、安装依赖、预览、测试并嵌入资源。在你授予的权限范围内，它还可以检查正在运行的应用、操作界面并调用指定的 C++ 函数。
- **锁定开发依赖。** 开发环境固定版本，经 SHA-512 校验后在首次使用时缓存。项目清单和锁文件记录团队与智能体共同使用的依赖。
- **为现有 C++ 软件添加界面。** 保留事件循环、工作线程、设备控制和 SIMD 或 CUDA 计算。界面可以打开在原生窗口或系统浏览器中，也可以同时运行多个应用或窗口。见[宿主集成指南](docs/HOST_INTEGRATION.md)。
- **直接传输数值数据。** 类型化数组和压缩位集通过二进制 WebSocket 桥接传输，无需转换成 JSON 或 Base64。见[异步执行模型](docs/ASYNC_EXECUTION.md)。
- **测试并打包应用。** 在 CMake 构建之外，可使用浏览器测试运行器和公开的 `webfront::bdd` 框架。嵌入最终的界面资源后即可离线分发。见[项目集成](#集成到现有项目)和[应用测试](#测试你的应用)。
- **自由选择界面技术。** 可以使用 React，也可直接用 C++ 创建 [DOM 岛](docs/DOM.md)，或根据 C++ 类型描述生成[表单和检查器](docs/FORMS.md)。

**网站与书籍** · [webfront.fr](https://webfront.fr/en/) · [WebFront 一书](https://webfront.fr/en/book/) · [文档](docs/) · [更新记录](docs/CHANGELOG.md)

## 平台与部署

| 目标平台 | 界面选项 | 验证范围 |
| --- | --- | --- |
| Windows | 系统浏览器、原生 WebView2、可选 CEF | MSVC 桌面 CI。WebView2 需要 Evergreen 运行时 |
| macOS 13.3 及以上（原生窗口） | 系统浏览器、系统 WKWebView、可选 CEF | AppleClang 桌面 CI |
| Linux | 系统浏览器、原生 WebKitGTK 6.0、可选 CEF | GCC 桌面 CI，另有 Clang 扩展验证 |
| Linux ARM64 / NVIDIA Jetson | 系统浏览器，以及平台支持的 WebKitGTK 或 CEF | GCC 13 ARM64 CI，并在 **Orin Nano Super 8 GB** 真机上验证。见 [Jetson 指南](docs/JETSON.md) |

核心采用标准 C++ 设施和可替换的网络实现。使用默认的系统浏览器时，无需链接原生 GUI 工具包或 WebView 引擎。目标平台需要合适的 **C++23 编译器和标准库、操作系统提供的网络与线程支持，以及浏览器**。GCC 13 是尽力支持的最低版本。已验证的工具链和支持级别见[支持策略](docs/adr/ADR-022-profils-et-niveaux-de-support.md)。

**无需单独安装 WebFront 运行时。** 核心遵循标准 C++ 的网络与执行模型。在相关设施逐步标准化并由标准库实现的过程中，CMake 在构建时提供固定版本的替代实现，包括持续维护的网络层 `stdnet`，以及按需提供 sender 支持的 `stdexec`。WebFront 本身以头文件形式提供。原生 WebView 仍需满足各平台的要求，CEF 可选。开发工具和软件包首次使用时经验证后存入缓存，嵌入了界面的发行版无需该缓存和界面源码。

### CUDA 与嵌入式应用

在 NVIDIA Jetson Orin Nano 等设备上，可以继续用 C++ 编写 CUDA 计算代码。宿主循环或工作线程负责在 CPU 或 CUDA 上计算，Web 界面调整参数，并通过类型化数组接收和显示结果。如果 CUDA 编译器有此要求，请将 `.cu` 编译单元与用到 WebFront C++23 头文件的编译单元分开。纯 CPU 应用无需 CUDA 工具包。

内置的 [Boing Ball 示例](docs/HOST_LOOP_EXAMPLE.md)同时支持 CPU 和 CUDA。棋盘格球在可反射的地面上方弹跳，切换到有色玻璃材质后，可以看到反射、折射和彩色光束。C++ 宿主逐像素计算，页面在 Canvas 上显示结果。原生窗口和系统浏览器共用同一宿主循环。

![Jetson Orin Nano 原生窗口中的 CUDA Boing Ball 动画，从棋盘格球切换为透光玻璃球](docs/images/host-loop-boing.gif)

```bash
cmake --build build --target WebFrontHostLoop --parallel 2
build/examples/WebFrontHostLoop --quality 0
# On a CUDA-equipped build and device:
build/examples/WebFrontHostLoop --engine cuda --glass --stats
```

从仓库根目录运行以上命令。Visual Studio 等多配置生成器的产物位于 `build/examples/Debug/`，以 `--config Release` 构建时则位于 `build/examples/Release/`。Windows 下的文件名是 `WebFrontHostLoop.exe`。

构建时会自动检测 CUDA。使用 `-DWEBFRONT_HOST_LOOP_CUDA=OFF` 可以在没有 CUDA 工具包时构建完整的 CPU 示例。运行时加上 `--browser` 可选择系统浏览器。质量选项、基准测试和单独的 CUDA 编译器配置见[示例指南](docs/HOST_LOOP_EXAMPLE.md)。

**Orin Nano Super 8 GB 真机验证**包括构建、CPU/CUDA 渲染、原生窗口和桥接测试，以及在沙箱中运行发行版的检查。受信任的[硬件验证工作流](docs/JETSON_RUNNER.md)补充了托管的 ARM64 CI。参阅[开发板验证](docs/JETSON.md)及[渲染测量](docs/HOST_LOOP_EXAMPLE.md)了解软件栈和覆盖范围。NX/AGX 和其他 JetPack 版本需要单独验证。上述结果未验证 WebKit 沙箱内的加速渲染。

Linux 上的 WebKitGTK 默认使用开发模式，沙箱处于关闭状态。分发时启用 `-DWEBFRONT_DISTRIBUTION=ON`，并在需要时随应用打包沙箱配置。见[开发与分发](docs/WEBKITGTK.md#development-and-distribution)。

## 性能设计

重计算留在原生代码中，界面只接收所需数据。数值缓冲区保留具体类型，位集保持压缩格式，桥接调用以异步方式执行。服务使用独立线程，计算循环和工作线程可以独立运行。可选的 React 适配器按浏览器合并 C++ 状态快照，DOM API 则批量提交写入。

这些机制有助于控制序列化、更新频率和界面开销。端到端性能仍取决于消息大小、渲染方式和目标硬件。应分别测量计算、传输和显示。关于各项工作所在的线程以及如何保持回调及时响应，见[执行指南](docs/ASYNC_EXECUTION.md)。

在 **25 W 的 Orin Nano Super 8 GB** 上，使用启用沙箱的 WebKitGTK 2.52.6 窗口，Boing Ball 的实测结果如下。

| 场景 | CPU（6 线程） | CUDA |
| --- | ---: | ---: |
| 不透明球 | 12 帧/秒 | 21 帧/秒 |
| 玻璃球和光束 | 约 0.6 帧/秒 | 12 帧/秒 |

场景为 960 × 720，每像素四条光线，窗口为 800 × 600 CSS 像素。使用 Release 构建、GCC 13.3、CUDA 13.2.86 和 L4T R39.2.1。这些是该开发板上的实测值。[测量报告](docs/HOST_LOOP_EXAMPLE.md#system-browser-and-distribution-follow-up)分别统计计算、复制、传输和显示耗时，并比较系统浏览器与原生窗口。

## 从哪里开始

- **与编程智能体一起工作**。参阅[开发流程](#与编程智能体协作)和[应用自动化](#让智能体操作你的应用)。
- **试用 WebFront**。从 [Hello World](#hello-world)开始，再体验[生命游戏教程](#从-c-模拟到可分发的生命游戏)。
- **为现有软件添加界面**。参阅 [CMake 集成](#集成到现有项目)和[宿主循环示例](examples/host-loop/HostLoop.cpp)。
- **准备投入生产**。参阅[测试](#测试你的应用)、[接口约定与限制](#接口约定与限制)和[支持状态](docs/PROJECT_STATUS.md)。

## 与编程智能体协作

使用编程智能体时，请先了解下面的开发流程。详细说明见[智能体开发指南](docs/DEVELOPING_WITH_AGENTS.md)。

- **项目结构。** WebFront 应用是带有 TypeScript/TSX 界面的 C++ 程序。将入口 `ui/main.ts` 或 `ui/main.tsx` 放在项目的 **`ui/`** 目录中。通过 `app.bootstrap("developer")` 打开应用内终端，再用 **`wf dev`** 开发界面。直接由 `documentRoot` 提供的手写 HTML 和普通 JavaScript 页面不在此工具链的处理范围内。
- **开发步骤。** 在固定版本的开发环境中，运行 `wf create app`（`app` 是模板名）、`wf init` 和 `wf install <package>`，再运行 `wf dev ui/main.ts`。准备交付时，按项目配置执行 `wf check`、`wf test` 和 `wf embed`。React 教程使用 `wf create life`。运行 `wf help agents` 可查看流程。
- **智能体通过 MCP 自行运行 `wf`。** 在开发版中调用 `webfront::automation::enableAutomation(app, {.grants = {...}})` 后，应用即成为 MCP 服务端。应用所有者决定授权范围，无需另启程序。应用会输出地址和私有客户端配置 `mcp.json` 的路径。文件含地址和 Bearer 令牌，存放在项目之外。

```text
WebFront MCP (grants: check, build, preview): http://127.0.0.1:PORT/mcp - client configuration: .../sessions/SESSION/mcp.json
```

```json
{"mcpServers": {"webfront": {"type": "http", "url": "http://127.0.0.1:PORT/mcp", "headers": {"Authorization": "Bearer TOKEN"}}}}
```

把配置文件交给 MCP 客户端，例如运行 `claude --mcp-config PATH/mcp.json`。请勿把令牌复制到项目中。智能体通过 `webfront.start` 发起 `create`、`preview`、`check`、`test` 或 `embed`，再用 `webfront.result` 读取结果。每次修改后用 `preview` 代替终端中的 `wf dev`。

在集成 WebFront 的项目中，`webfront_write_agent_guide()` 将这些规则写入项目根目录的 `AGENTS.md`。`wf create` 也会在生成的源码旁写入这份指南。

## 从 C++ 到响应式界面

先注册模拟函数，再启动开发环境，最后在应用内终端中构建界面。`life-project` 是存放界面源码的工作目录。

<details>
<summary>动画中的 C++ 应用源码</summary>

```cpp
#include <webfront/WebFront.hpp>
#include <bitset>

using namespace std;
constexpr int N = 32;
using Grid = bitset<N * N>;

Grid nextLife(const Grid& cells) {
    Grid next;
    auto index = [](int x, int y) {
        return size_t((y + N) % N * N + (x + N) % N);
    };
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            int n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (dx || dy)
                        n += cells[index(x + dx, y + dy)];
            auto i = index(x, y);
            next[i] = n == 3 || (cells[i] && n == 2);
        }
    return next;
}

int main() {
    webfront::NativeApp app{"life-project"};
    app.cppFunction("life.step", &nextLife);
    app.bootstrap("developer");
    app.show();
}
```

</details>

## Hello World

最小示例会提供内置页面，并用系统浏览器打开它。见 [HelloWorld.cpp](examples/hello-world/HelloWorld.cpp)。

```cpp
#include <webfront/WebFront.hpp>

int main() {
    webfront::WebFront app;
    app.show();
}
```

需要 Git、CMake **3.31+** 和支持 C++23 的编译器。首次配置、首次启动开发环境和首次安装包需要联网。下载完成后，开发环境和依赖会保留在缓存中。见[编译器支持策略](docs/adr/ADR-022-profils-et-niveaux-de-support.md)及 [LTS 发行版构建工具](#lts-发行版上的构建工具)。

```bash
mkdir build
cd build
cmake ..
cmake --build .
examples/HelloWorld
```

使用 Visual Studio 的多配置生成器时，运行 `examples\Debug\HelloWorld.exe`。如需 Release 版本，构建时传入 `--config Release`，再从 `examples\Release\` 启动。

在终端按 Enter 停止服务器。

同一个应用也能通过 [DOM API](docs/DOM.md) 直接在 C++ 中构建页面，无需另备界面文件。先注册处理函数，再调用 `show()`。服务器停止后，`show()` 才返回。

```cpp
#include <webfront/WebFront.hpp>

int main() {
    webfront::WebFront app;
    app.onUIStarted([](auto ui) {
        auto doc = ui.document();
        auto root = doc.island(); // default: "root"
        auto button = doc.element("button");
        auto label = doc.text("Click me");
        button.append(label);
        button.listen("click", [label](std::string) { label.setText("Clicked"); });
        root.append(button);
    });
    app.show();
}
```

## 从 C++ 模拟到可分发的生命游戏

生命游戏示例先用 C++ 计算，再添加 React 界面，最后将界面嵌入可执行文件。以下命令从上一步的 `build` 目录运行。

```bash
git clone https://github.com/ambroise-leclerc/WebFront.git
cd WebFront
mkdir build
cd build
cmake ..
cmake --build .
```

上面的构建无需额外选项。配置时如果找到本平台的原生窗口依赖，就会启用相应引擎。见[第 2 步](#2-打开应用内的开发环境)。使用 Visual Studio 等多配置生成器时，示例位于 `examples\Debug\`，传入 `--config Release` 后则位于 `examples\Release\`。

### 1. 运行 C++ 模拟

[Life.hpp](examples/life/Life.hpp) 在首尾相接的 32 × 32 网格上实现 Conway 生命游戏。每一代都用一个 `std::bitset<1024>` 表示。[LifeConsole.cpp](examples/life/LifeConsole.cpp) 可在终端中直接运行模拟。

```bash
examples/WebFrontLifeConsole
```

### 2. 打开应用内的开发环境

[LifeDeveloper.cpp](examples/life/LifeDeveloper.cpp) 注册模拟函数，并启动应用内的开发环境。

```cpp
#include "LifeFrontend.hpp"

webfront::demo::LifeApp app{"life-project"};
app.cppFunction("life.step", &webfront::demo::nextLife);
app.bootstrap("developer");
app.show();
```

```bash
examples/WebFrontLifeDeveloper
```

默认工作目录为 `build/life-project`。目录不存在时会创建，已存在时会原样复用。以下 `life-project/...` 路径都以该项目目录为基准。启动失败时示例返回非零退出状态。

`bootstrap("developer")` 打开集成终端，加载固定版本的开发环境并校验 SHA-512。下载内容会保留在缓存中，后续可离线启动。终端提供面向项目的 `wf` 命令，以及用于查看项目的 `ls`、`cat`、`tree` 和 `mounts`。终端不提供系统 Shell，宿主机也无需安装 JavaScript 工具链。

**原生窗口。** 配置时，CMake 会自动检测当前平台的原生引擎。

| 平台 | 引擎 | 检测条件 |
| --- | --- | --- |
| Windows | Microsoft Edge WebView2 Evergreen | 使用兼容 MSVC 的编译器。构建时下载 SDK，启动时检查 Evergreen 运行时 |
| macOS 13.3+ | 系统 Cocoa / WebKit | 使用 Clang，部署目标至少为 13.3 |
| Linux | WebKitGTK 6.0 | `pkg-config webkitgtk-6.0` 成功。见[安装说明](docs/WEBKITGTK.md) |

macOS 使用系统 Cocoa 和 WebKit 框架打开 WKWebView，无需下载引擎。应用在主线程构造并调用 `show()`，HTTP/WebSocket 服务在独立线程上运行。关闭窗口后，服务停止，示例退出。显式选择低于 13.3 的部署目标会关闭 WKWebView 自动检测。在这种情况下手动启用 WKWebView 会导致配置失败。见 [macOS 指南](docs/WKWEBVIEW.md)。

找不到原生引擎时，配置仍会成功，并提示应安装哪些依赖。示例会使用系统浏览器打开同一个应用。安装所需依赖后重新运行 `cmake ..` 即可构建原生窗口版。设定 `-DWEBFRONT_EMBED_CEF=ON` 可以为自己的应用启用 `CEFFrontend`。这些示例使用平台原生引擎或系统浏览器。

<details>
<summary>原生引擎和开发环境的高级选项</summary>

- 用 `-DWEBFRONT_EMBED_WEBVIEW2=ON|OFF`、`-DWEBFRONT_EMBED_WKWEBVIEW=ON|OFF` 或 `-DWEBFRONT_EMBED_WEBKITGTK=ON|OFF` 手动选择。显式设置优先，`-DWEBFRONT_AUTODETECT_NATIVE=OFF` 可禁用自动检测。测试构建（`-DWEBFRONT_ENABLE_TESTING=ON`）保留显式选择。
- 每次配置都会重新检测，自动检测的结果不会写入缓存中的引擎选项。要在 macOS 上恢复自动检测，请运行 `cmake .. -UWEBFRONT_EMBED_WKWEBVIEW`。其他平台可清除对应的引擎选项。
- 也可用 `examples/WebFrontLifeDeveloper <project> [<environment-url> <sha512-integrity>]` 传入其他项目目录和环境归档，例如[从源码构建的环境](docs/DEVELOPER_ENVIRONMENT.md#environment-distribution-and-offline-tests-maintainers)。

</details>

Visual Studio 的示例可执行文件位于 `examples\Debug\`，Release 版本位于 `examples\Release\`。

### 3. 创建项目并安装依赖

在应用内终端中运行以下命令。

```text
wf create life
wf init
wf install react@19.3.1 react-dom@19.3.1
wf dev ui/main.tsx
```

- `wf create life` 将可编辑的 [TSX 和 CSS 模板](examples/life/ui)写入 `ui/`，已有目录会保留。参数是模板名。`wf create app` 可为新项目生成最小的 TypeScript 界面。
- `wf init` 写入 `webfront.project.json`。其他命令从该清单读取依赖、入口、资源提供器、脚本和权限。
- `wf install` 从包仓库解析依赖，校验完整性，并将确切版本写入 `webfront-packages.lock.json`。安装过程不会执行软件包的安装脚本。`wf restore` 可在其他机器上按锁文件重建缓存，`wf restore --offline` 可离线验证缓存。
- `wf dev` 用 esbuild-wasm 编译 TSX，在应用中打开界面，并在保存源码后重新构建。构建错误会显示在终端和预览上，预览保留最近一次成功的版本。按 Ctrl+C 停止。若只需编译一次，可运行 `wf build`。

用常用编辑器修改 `life-project/ui/main.tsx` 和 `style.css`。React 绘制网格和控件，每一代的计算都等待 C++ 回调完成。

```js
await webFront.ready;
const step = webFront.cppFunction('life.step');
const next = await step(cells);
```

`cells` 和 `next` 都是 128 字节的 `Uint8Array`，对应 C++ 的 `std::bitset<1024>`，每字节从最低有效位开始。JavaScript 调用 C++ 始终返回 Promise，需等待结果或处理拒绝。生命游戏的规则由 C++ 执行。

### 4. 运行测试并检查类型

`wf test` 用锁文件指定的 `jasmine-core` 版本在应用窗口中运行 [Jasmine](https://jasmine.github.io/) 测试。安装依赖，创建 `life-project/tests/life.spec.js`，并在 `webfront.project.json` 中列出该文件。`wf init` 不会复制模板的测试配置。

```js
describe('the Life grid', () => {
    it('packs 32 by 32 cells into 128 bytes, least significant bit first', () => {
        const cells = new Uint8Array(32 * 32 / 8);
        for (const i of [330, 331, 332]) cells[i >> 3] |= 1 << (i & 7);
        expect(cells.length).toBe(128);
        expect(cells[41]).toBe(0b00011100);
    });
});
```

```text
wf install jasmine-core@7.0.2
```

```json
"test": {"specs": ["tests/life.spec.js"]}
```

```text
wf test
```

测试失败时，命令返回非零退出状态。

构建时会移除 TypeScript 类型，但不会进行类型检查。`wf check` 在浏览器中运行项目锁定版本的 TypeScript 编译器。它需要 `tsconfig.json` 和导入项的类型声明包。模板和集成测试使用以下配置。

```json
{
  "compilerOptions": {
    "strict": true,
    "target": "ES2022",
    "module": "ESNext",
    "moduleResolution": "Bundler",
    "jsx": "react-jsx",
    "lib": ["ES2022", "DOM", "DOM.Iterable"],
    "noEmit": true
  },
  "include": ["ui"]
}
```

运行 `wf install typescript@5.9.3 @types/react@19.3.0 @types/react-dom@19.3.0` 安装类型相关依赖，然后运行 `wf check`。见[开发环境参考](docs/DEVELOPER_ENVIRONMENT.md)。

### 5. 体验界面

![原生窗口中的生命游戏](docs/images/life-preview.jpg)

可以播放、暂停、单步演进、绘制细胞、切换图案、调整速度或切换到俯视图。代数和活细胞计数来自 C++ 模拟。同一时刻最多有一个演进请求。请求失败时，播放停止，界面显示错误。

### 6. 嵌入界面以供分发

在 `webfront.project.json` 中声明入口和资源提供器（`"entries": {"main": "ui/main.tsx"}` 和 `"providers": {"LifeUI": {"entry": "main", "output": "generated/LifeUI.hpp"}}`），也可以在应用内终端中直接传入参数。

```text
wf embed ui/main.tsx --output generated/LifeUI.hpp
```

此命令构建界面，并将资源和依赖许可声明打包到压缩的 C++ 头文件中。关闭开发窗口，然后在原来的 `build` 目录中重新配置。此前选择的引擎会保留。

```bash
cmake .. -DWEBFRONT_LIFE_ASSETS="$PWD/life-project/generated/LifeUI.hpp"
cmake --build . --target WebFrontLifeEmbedded
examples/WebFrontLifeEmbedded
```

使用 Visual Studio 时，运行 `examples\Debug\WebFrontLifeEmbedded.exe`。

[LifeEmbedded.cpp](examples/life/LifeEmbedded.cpp) 用生成的资源提供器替代开发项目和 bootstrap 调用。

```cpp
#include "LifeFrontend.hpp"

webfront::demo::LifeAppWithAssets<webfront::fs::LifeUI> app;
app.cppFunction("life.step", &webfront::demo::nextLife);
app.show();
```

重新生成资源后需重新编译。最终应用可以离线运行，无需项目目录、开发回调或已下载的工具缓存，仍需满足所选原生引擎的部署要求。安装包由应用自行制作。

两个阶段均由 [LifeFrontend.hpp](examples/life/LifeFrontend.hpp) 选择 `NativeOrBrowserFrontend`。构建中包含原生引擎且存在可用的图形会话时，使用原生窗口，否则使用默认浏览器。需要明确使用原生窗口时，自己的应用可以选择 `webfront::NativeApp` 或 `webfront::NativeAppWithAssets`。

## 让智能体操作你的应用

编程智能体可以在**你的应用**自己的窗口中检查类型、构建、预览和检查界面，点击控件，并调用你授权的 C++ 回调。你保留对窗口和权限的控制。自动化默认关闭。

<img src="docs/images/automation-agent.svg" width="100%" alt="左侧智能体通过应用的 MCP 地址检查、构建和预览界面，点击按钮，调用已授权的 bf.render，随后被拒绝调用 bf.secret。右侧应用窗口更新，C++ 进程只记录实际执行的回调。">

*这是 `AutomationConsumerWindow` 回归测试的节选。测试在真实应用中执行这些步骤。*

在 C++ 中调用以下接口启用自动化，权限由你在原生代码中设定。

```cpp
#include <webfront/automation/Enable.hpp>

webfront::NativeApp app{project};
app.bootstrap("developer");
app.cppFunction("bf.render", render);                  // your callbacks stay yours
auto automation = webfront::automation::enableAutomation(
    app, {.grants    = {"check", "build", "preview", "inspect", "interact", "call"},
          .callbacks = {"bf.render"}});                // the only callback an agent may call
app.show(automation->launchPage());
automation->close();
app.shutdown();
```

- **可见。** 应用输出 MCP 地址，窗口中的终端显示授权信息，`wf help` 也会提示。还可以在窗口标题中加上标记。
- **可发现。** 各运行实例将地址和私有 `mcp.json` 的路径写入用户级记录。令牌存于配置文件，不出现在发现记录里。可用 `claude --mcp-config PATH/mcp.json` 或兼容的 MCP 客户端连接。`wf create` 生成的 `AGENTS.md` 会说明连接方法。
- **有明确边界。** 每项操作单独授权，回调只可通过列出的确切名称调用。原生代码用超时机制停止卡住的渲染器。项目代码仍以用户权限执行，自动化不提供沙箱。
- **按需启用。** 接口集中在一个头文件中。未调用 `enableAutomation` 的应用不会注册自动化功能或监听连接。发行构建（`WEBFRONT_DISTRIBUTION`）禁止编译此功能。

权限、回调、发现机制、超时和 MCP 配置见[自动化指南](docs/AUTOMATION.md#automating-your-own-application)。

## 生产项目功能

除基本桥接外，WebFront 还提供[由应用管理生命周期的协程及取消机制](docs/ASYNC_EXECUTION.md)、向已连接浏览器广播消息、[C++ DOM 岛](docs/DOM.md)、[表单及 TypeScript 声明](docs/FORMS.md)，以及可选的 [React store 和 `Island` 适配器](docs/REACT.md)。各组件可独立采用，核心无需 React。

版本历史、迁移说明和破坏性变更见[更新记录](docs/CHANGELOG.md)。为生产环境选择平台或前端前，可查看[项目状态](docs/PROJECT_STATUS.md)中的验证范围和限制。

## LTS 发行版上的构建工具

独立构建和通过 `add_subdirectory` 或 CPM 集成都要求 CMake **3.31+**，即使关闭测试也是如此。Ubuntu 22.04 自带 CMake 3.22.1，Ubuntu 24.04 自带 3.28.3，请先升级 CMake。Ubuntu 22.04 自带的 Git 2.34.1 可以运行仓库的提交检查。统一最低版本的依据见[工具链决策与审查](docs/adr/ADR-022-profils-et-niveaux-de-support.md#outils-de-construction-distributions-lts-527)。可选的 mddlog 有[独立要求](docs/mddlog-integration.md)。

可以使用隔离的 Python 环境。Ubuntu 上如有需要，请先运行 `sudo apt-get install python3-venv`，然后执行以下命令。

```bash
python3 -m venv "$HOME/.local/share/webfront-tools"
"$HOME/.local/share/webfront-tools/bin/python" -m pip install 'cmake>=3.31,<4'
export PATH="$HOME/.local/share/webfront-tools/bin:$PATH"
cmake --version
ctest --version
```

安装这份 CMake 不需要 root 权限。每次打开构建 Shell 时运行 `export PATH=...`，或将其加入 Shell 配置，以选用配套的 `cmake` 和 `ctest`。也可按 Ubuntu 版本使用 [Kitware APT 仓库](https://apt.kitware.com/)安装。升级 CMake 后，Ubuntu 22.04 默认的 GCC 11 仍需换成支持策略列出的工具链。

## 集成到现有项目

WebFront 的实现位于 `include/`。CMake 提供所需的 C++ 依赖和平台配置。WebFront 以头文件提供，无需生成库二进制文件。

```cmake
add_subdirectory(path/to/WebFront)
add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE WebFront::WebFront)
```

作为子项目使用时，示例和测试默认关闭，WebFront 的编译选项不会影响你的目标。独立构建也可通过 `WEBFRONT_BUILD_EXAMPLES=OFF` 关闭示例。

<details>
<summary>GCC 16 与 C++ 模块扫描</summary>

在 CMake 扫描 C++ 模块并添加 `-fmodules-ts` 时，GCC 16 会在 stdexec 的 `__query.hpp` 中发生内部编译器错误（段错误）。对于 Ninja 上的 C++20 及更高版本目标，CMake 默认会扫描模块（CMP0155）。WebFront 仅使用头文件，不需要模块扫描。对链接 WebFront 且不使用 C++ 模块的目标，可在创建目标后关闭模块扫描。

```cmake
add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE WebFront::WebFront)
webfront_disable_module_scan(MyApp)
```

如果项目中的所有目标都不使用 C++ 模块，也可以在创建目标前设置 `set(CMAKE_CXX_SCAN_FOR_MODULES OFF)`。否则 CMake 无法确定模块的编译顺序。显式设置源文件扫描属性或包含 `CXX_MODULES` 文件集的目标仍会被扫描。

用 GCC 16 将 WebFront 配置为子项目并保留扫描时，CMake 会列出受影响的目标及相应的解决方法。

</details>

`webfront::WebFront` 始终使用系统浏览器。若要和生命游戏示例一样使用原生窗口并保留浏览器回退，可在链接目标后添加以下调用。它也适用于 `CPMAddPackage`。

```cmake
webfront_use_native_window(MyApp)
```

在 `main.cpp` 中根据辅助函数定义的宏选择应用类型。

```cpp
#include <webfront/WebFront.hpp>

#if defined(WEBFRONT_NATIVE_WINDOW)
using App = webfront::WebFrontWithFrontend<webfront::frontend::NativeWebViewFrontend>;
#else
using App = webfront::WebFront;
#endif

int main() {
    App app;
    app.show();
}
```

若要在有图形会话时打开原生窗口，并在没有可用显示环境时（例如 SSH、容器或 CI）回退到系统浏览器，可使用 `WebFrontWithFrontend<webfront::frontend::NativeOrBrowserFrontend>`。这个类型无需 `#if`，回退原因只输出一次。见[前端选择](docs/FRONTENDS.md)。

除了 WebFront 自身的测试和模糊测试构建（`WEBFRONT_ENABLE_TESTING=ON` 或 `WEBFRONT_ENABLE_FUZZING=ON`），CMake 默认自动检测原生引擎。父项目的 `ENABLE_TESTING` 和 `ENABLE_FUZZING` 不影响检测或 WebFront 的测试。启用引擎后，辅助函数为目标配置引擎并定义 `WEBFRONT_NATIVE_WINDOW`。否则，CMake 会给出安装提示，应用使用默认浏览器。Windows 需要兼容 MSVC 的编译器，CMake 会下载固定版本的 WebView2 SDK。Evergreen 运行时需单独安装。Linux 需要 WebKitGTK 6.0 开发文件，macOS 需要 Clang 和 macOS 13.3 或更高版本。显式设置的 `WEBFRONT_EMBED_<ENGINE>=ON|OFF` 优先，`WEBFRONT_AUTODETECT_NATIVE=OFF` 可禁用自动检测。默认流程无需在 `add_subdirectory` 或 CPM 之前设置选项。

父项目或其他目录也可以调用 `webfront_native_frontend_option(out)` 和 `webfront_setup_frontends_for_target(MyApp)` 来显式配置引擎。后者为目标配置所有已启用的引擎，原生窗口辅助函数只选择当前平台的原生引擎。见 [WKWebView](docs/WKWEBVIEW.md)、[WebKitGTK](docs/WEBKITGTK.md) 和[架构决策](docs/adr/README.md)。

## 测试你的应用

可选的 [Jasmine 与 CTest 集成](docs/CONSUMER_TESTING.md)在浏览器中针对应用页面运行测试。测试可以捕获 PNG 截图和图像序列，也可通过 `PROGRAMS` 运行测试所有者明确授权的原生程序，并用私有临时目录存放测试文件。运行原生程序的浏览器集成在 Linux CEF 上验证，平台范围和限制见指南。授权程序以用户权限运行，请使用可信的测试代码。

C++ 测试可以使用 WebFront 自己采用的公开框架 [`webfront::bdd`](docs/BDD.md)。链接 `WebFront::Bdd` CMake 目标，并调用 `webfront_bdd_discover_tests()`，可为每个场景注册一项 CTest 测试。

要显式选择嵌入式引擎，可用 `webfront::embeddedFrontendAvailable` 判断构建是否包含这类引擎，并用 `webfront::EmbeddedWebFront` 选择它。见[前端选择](docs/FRONTENDS.md)。

## 构建和测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=OFF -DWEBFRONT_ENABLE_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 4
```

启用测试时，配置步骤会下载固定版本的 bootstrap 环境和测试包仓库的包，并校验 SHA-512。嵌入资源仅用 CMake 生成和检查，可通过 `webfront-embedded-assets` 目标重新生成。常规 C++ 构建使用版本库中的头文件。浏览器测试需要启用引擎，Linux 上的 CEF 测试使用 Xvfb。见 [AGENTS.md](AGENTS.md) 和[开发环境测试](docs/DEVELOPER_ENVIRONMENT.md#environment-distribution-and-offline-tests-maintainers)。

上面的 CTest 命令最多并行运行四项任务。独立的驱动会话使用操作系统分配的端口，共享项目与嵌套构建使用资源锁。见[测试耗时与并行](docs/TEST_PERFORMANCE.md)。`ctest -L fast` 约一秒内运行快速单元测试。[测试分层](docs/TEST_TIERS.md)说明了每次拉取请求、每周以及合并到 `master` 前运行的检查。

CI 在 Windows、macOS 和 Linux 上验证 C++ 与原生浏览器路径。扩展任务覆盖 Clang、sanitizer、模糊测试、CEF 和 Linux ARM64，包括 ARM64 上的 CEF 与 WebKitGTK 浏览器测试。具体调度、工具链及选项见[测试分层](docs/TEST_TIERS.md)和 [CI 工作流](.github/workflows/BuildAndTest.yml)。托管的 ARM64 验证与 [Jetson 真机验证](docs/JETSON.md)分别进行。各平台徽章标明验证矩阵，CI 徽章反映总体状态。

## 接口约定与限制

- WebFront 在后台线程运行 HTTP/WebSocket 服务，回调在该线程逐个执行，避免在回调中阻塞线程。从任意线程都可以调用浏览器函数（`jsFunction`、`app.broadcast(name)(...)`），这些调用不会等待服务器线程。WKWebView、WebKitGTK 和 CEF 等前端通常要求在主线程创建并显示窗口，WebView2 可使用次线程。保留自身循环的应用可使用 `app.start()`（打开系统浏览器并立即返回）、`app.requestClose()` 或 `webfront::UIThread`。应用协程使用 `app.spawn(task)` 或 `ui.spawn(task)`。见[线程规则](docs/ASYNC_EXECUTION.md#threads)、[宿主集成](docs/HOST_INTEGRATION.md)及[异步执行与取消](docs/ASYNC_EXECUTION.md)。
- JavaScript 调用 C++ 始终返回 Promise。C++ 调用 JavaScript 时，`ui.jsFunction(name)(...)` 只发出调用，`ui.jsFunction<R>(name)(...)` 则返回 `std::future<R>`。函数不存在、异常、答复格式错误或连接断开都会使带返回类型的调用失败。
- 二进制桥接支持标量、字符串、元组、数值类型化数组、压缩位集及受支持的复合值的同构列表。任意对象不能通过桥接传输。字符串和消息受消息大小策略限制，默认上限为 64 MiB。达到 65,536 字节的字符串使用长字符串编码。见[二进制桥接约定](docs/adr/ADR-003-protocole-binaire-et-bornes.md)。
- 开发项目需显式开放访问，适用于可信本地代码。下载的工具拥有应用的桥接权限，脚本授权仅约束 `context` API，两者均不提供沙箱。只支持能在浏览器中运行的包，安装脚本和原生扩展不会执行。编译 TypeScript 时只转换代码，类型检查由 `wf check` 使用项目锁定版本的编译器单独运行。
- 可选的[原生伪终端](docs/PTY.md)在 Linux、macOS 和 Windows 上将应用指定的程序连接到 xterm.js。
- 可选的 [`@webfront/react` 适配器](docs/REACT.md)提供合并更新的 C++ store 和空的 React 岛。`WebFrontLifeReact` 演示了在生命游戏的 React 界面中嵌入 C++ 面板。用于发布到包仓库的归档在本地生成。
- 嵌入式 Linux 有 ARM64 CI 和特定 Orin Nano 真机验证。其他开发板、裸机和微控制器没有通用的验证覆盖。请确认目标的 C++23、内存、网络和浏览器要求。HTTPS 和 WSS 见[可选 TLS 监听器及远程部署说明](docs/TLS.md)。客户端身份验证由应用负责。
- 打包、无障碍和原生引擎部署仍需按应用处理。可选的 [`ui.document()` API](docs/DOM.md)提供 DOM 岛、自动批量写入、事件回调，以及使用 sender 的分组读取。已知限制见[项目状态](docs/PROJECT_STATUS.md)。

## 许可证

WebFront 采用 **Apache-2.0 或 CECILL-B** 双许可证，可任选其一使用、修改和再分发。[LICENSE](LICENSE) 包含 Apache 2.0 和 CeCILL-B 第 1 版全文，后者提供英语和法语版本。

第三方组件保留各自的许可证和声明。WebFront 的许可选择不改变其依赖项或随附第三方代码的许可。

[网站](https://webfront.fr/en/) · [书籍](https://webfront.fr/en/book/) · [开发环境参考](docs/DEVELOPER_ENVIRONMENT.md) · [架构决策](docs/adr/README.md) · [贡献指南](.github/CONTRIBUTING.md)
