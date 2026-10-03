# WindowsWidget · 随行组件

面向 Windows 11 的 C++ / WinUI 3 组件框架。打开通知中心或快速设置时，在系统面板旁显示独立组件卡片，收起后保持桌面干净。

当前可运行版本是带主控制中心的 Notification Companion：每个组件独立运行，支持用户添加零代码组件和独立 DLL MOD；已接入现有 DLL SDK。

```powershell
./demo/NotificationCompanion/build.ps1 -Test -Package
./out/companion-demo/ControlCenter/WindowsWidget.exe
```

EXE 安装包：运行 `./build-installer.ps1 -Version 0.1.0`，生成 `out/installers/WindowsWidget-0.1.0-x64-Setup.exe`。默认按当前用户安装，升级和卸载保留用户数据；详见 [安装器说明](installer/README.md)。

请先退出旧 Demo；各版本共用单实例锁。默认打开主控制中心，关闭窗口后驻留托盘，**Win+N / Win+A** 打开系统面板，**Ctrl+Alt+W** 手动预览，**Ctrl+Alt+Q** 退出。

主界面可隐藏托盘图标、启用开机自启动及选择静默启动，托盘右键保留打开控制中心与退出。点击主窗口关闭按钮后组件仍正常跟随通知中心；静默登录只驻留托盘，普通双击仍打开主窗口。自启动默认关闭，详见 [启动设置](docs/STARTUP.md)。

新版会在系统面板接管后结束手动预览，系统面板关闭时所有组件一起隐藏；切换排列和启停不会把组件变成常驻预览。

新版提供天气、待办、系统状态、常用入口四张卡片，没有时钟、日历或专注计时。主控制中心直接提供启停、设置、重启、顺序和排列入口。可创建自己的便签、文字和清单，也可导入 HTTPS 链接组件。卡片采用原生 Acrylic、8 DIP 间距和统一动画，默认从通知中心旁的右下角排列；关闭自动排列后可拖动顶部标题，自由位置、待办和城市会保存。支持中英文、系统明暗主题、组件启停和位置重置。

天气来自 [Open-Meteo](https://open-meteo.com/en/docs)，支持更换城市与网络失败状态。待办支持添加、勾选和删除，文字编辑使用独立普通窗口。

系统面板识别和动画没有稳定公开保证；自动跟随要求明确身份，单条消息横幅不会触发。隐藏自测覆盖布局、窗口和动画逻辑，实际系统同步、轻交互、多屏和全屏行为仍需实测。

| 文档 | 内容 |
| --- | --- |
| [使用说明](demo/NotificationCompanion/README.md) | 新版卡片、构建与操作 |
| [产品设计](docs/PRODUCT.md) | 外观、布局与范围 |
| [架构](docs/ARCHITECTURE.md) / [触发](docs/TRIGGERS.md) | 框架目标与识别策略 |
| [SDK](docs/SDK.md) / [插件接入](docs/PLUGIN_DEV.md) | 接口与兼容边界 |
| [自定义组件与 Agent 开发指南](sdk/components/README.md) | 组件设计、限制、开发提示词与交付验收 |
| [手动测试](docs/MANUAL_TESTS.md) / [验证记录](docs/VALIDATION.md) | 检查与限制 |
| [变更](docs/CHANGES.md) / [依赖许可](docs/THIRD_PARTY.md) | 交付记录 |

重构前文档保存在 [历史档案](docs/archive/2026-10-02-before-companion/INDEX.md)。
