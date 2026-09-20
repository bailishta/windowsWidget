# 第三方依赖

精确版本在项目根目录 `packages.config` 中锁定，使用 NuGet 官方源还原。构建输出包含 Microsoft Windows App SDK 自包含运行时及 Visual C++ Redistributable 的应用本地运行库。

- Windows App SDK / WinUI：Microsoft 的 Windows App SDK 许可及各组件附带许可，见还原包中的许可证与 `.nuspec` 许可字段，以及 https://github.com/microsoft/WindowsAppSDK 。
- C++/WinRT：Microsoft，MIT License，见 https://github.com/microsoft/cppwinrt 。
- WebView2：作为 WinUI 构建引用引入，见对应 NuGet 包许可；本项目不创建 WebView2 控件。
- VC Runtime：按 Visual Studio 的可再分发代码条款随应用分发。

未复制第三方桌面嵌入项目的源码。桌面适配层直接使用 Win32 API 和 Explorer 窗口探测实现。
