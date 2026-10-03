# 依赖与许可

独立 demo 复用 packages.config 固定版本，无新 UI 框架或生产工具链升级。

| 依赖 | 版本 | 来源 |
| --- | --- | --- |
| Windows App SDK | 1.8.260804001 | [微软仓库](https://github.com/microsoft/WindowsAppSDK)，以包内许可为准 |
| WinUI 传递依赖 | 1.8.260803003 | Windows App SDK |
| C++/WinRT | 2.0.250303.1 | [微软仓库](https://github.com/microsoft/cppwinrt)，MIT |
| Windows SDK BuildTools | 10.0.26100.4654 | 微软构建工具许可 |
| MSIX BuildTools | 1.7.20250829.1 | 微软构建工具许可 |
| WebView2 SDK | 1.0.3179.45 | [微软 WebView2](https://developer.microsoft.com/microsoft-edge/webview2/)，包内许可 |
| Inno Setup（安装器构建工具） | 本次使用 7.1.0 x64 | [官方许可](https://jrsoftware.org/files/is/license.txt)，编译器不随应用分发 |

全部传递依赖见根目录与 demo 的 packages.config。WebView2 属现有构建链依赖，demo 没用浏览器控件。

目录产物包含 Windows App SDK 自包含运行文件和 VS x64 Microsoft.VC143.CRT DLL；后者遵守微软再发行条款。构建工具、Windows SDK 和 Debug CRT 不随分发。

发布应保留适用许可/notice，核对微软再发行条款。-Package 复制完整运行目录和本说明，不等于经过正式发布许可审计。

原型构建会将固定 NuGet 包顶层的 license/notice 文件按包名复制到运行目录 licenses，目录包同时包含这些原文。

Windhawk 仅是研究资料，没有复制代码或采用注入路径。[依据](RESEARCH.md)、[旧许可记录](archive/2026-10-02-before-companion/THIRD_PARTY.md)。

天气服务与地理编码使用 [Open-Meteo](https://open-meteo.com/en/docs)，卡片保留来源链接。天气数据采用其公开数据许可；免费 API 的用途和流量限制以 [服务条款](https://open-meteo.com/en/terms) 为准，商业发布应另行核对。
