# 资料依据
核对日期：2026-10-02。聊天用于设计参考，不替代文档和实测。

设计参考：[设计 Win11 组件](https://chatgpt.com/c/6abf551b-b9a4-83ee-9644-fc95471cc84d)。其中的助手解释按假设处理，以下是独立核对依据。

| 官方 API 结论 | 微软文档 |
| --- | --- |
| NOACTIVATE 改变点击时激活；TOOLWINDOW 不进入任务栏/Alt+Tab | [扩展样式](https://learn.microsoft.com/en-us/windows/win32/winmsg/extended-window-styles) |
| MA_NOACTIVATE 不激活且不丢弃鼠标消息 | [WM_MOUSEACTIVATE](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-mouseactivate) |
| TOPMOST 高于非置顶；NOACTIVATE 不激活 | [SetWindowPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos) |
| cloak 需独立查询，可见标志不能单独说明呈现状态 | [DWM 属性](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute) |
| Win+N / Win+A 的入口应区分 | [Windows 快捷键](https://support.microsoft.com/en-us/accessibility/windows/keyboard-shortcuts-in-windows) |
| 跨进程重挂接存在 DPI 约束 | [SetParent](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setparent) |
| Button 等控件会处理底层指针事件，诊断需观察已处理事件 | [WinUI AddHandler](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.ui.xaml.uielement.addhandler?view=windows-app-sdk-1.8) |
| 非打包应用语言覆盖使用 Windows App SDK 的 Globalization API | [PrimaryLanguageOverride](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.windows.globalization.applicationlanguages.primarylanguageoverride?view=windows-app-sdk-1.8) |
| EnumWindows 在 Windows 8 起仅承诺枚举桌面应用顶层窗口，需要补充候选发现 | [EnumWindows](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumwindows) |
| Shell 线程窗口枚举与类名查找属于公开 Win32 方法 | [EnumThreadWindows](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumthreadwindows)、[FindWindowEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-findwindowexw) |
| UIA 跨进程查询应避开自己的 UI 线程，使用 MTA 工作线程 | [UIA 线程](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-threading)、[UIA 元素](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nn-uiautomationclient-iuiautomationelement) |
| 可以取得 XAML 元素的合成 Visual；它与整窗系统背景的动画不是同一目标 | [GetElementVisual](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.ui.xaml.hosting.elementcompositionpreview.getelementvisual?view=windows-app-sdk-1.8)、[XAML 与合成](https://learn.microsoft.com/en-us/windows/apps/develop/composition/xaml-comp-interop) |
| UISettings 可查询用户是否启用界面动画 | [AnimationsEnabled](https://learn.microsoft.com/en-us/uwp/api/windows.ui.viewmanagement.uisettings.animationsenabled) |
| Acrylic 是临时弹出表面的半透明材质；自定义染色参数会停止默认明暗配方跟随 | [系统背景](https://learn.microsoft.com/en-us/windows/apps/develop/ui/system-backdrops) |
| 默认背景配置按连接上下文维护；Theme、IsInputActive、高对比度由上下文提供 | [GetDefaultSystemBackdropConfiguration](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.ui.xaml.media.systembackdrop.getdefaultsystembackdropconfiguration?view=windows-app-sdk-1.8) |
| 标准动画资源提供 250/167/83ms，Fluent 入场/退场基准曲线分别是 (0,0,0,1)/(1,0,1,1) | [时长与缓动](https://learn.microsoft.com/en-us/windows/apps/design/motion/timing-and-easing) |
| DwmFlush 等待调用进程提交的渲染更新，并不刷新整个会话的渲染批次 | [DwmFlush](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmflush) |
| 通知横幅与通知中心中的消息是不同呈现位置 | [Windows 通知说明](https://support.microsoft.com/en-us/windows/experience/notifications-and-do-not-disturb-in-windows) |

这些没有保证通知中心外部点击不关闭，也没有保证普通置顶超过 Shell 特殊层级、独占全屏或安全桌面。

本机诊断实测早期 SystemBackdrop 配置回调可能得到空默认配置；原型增加空值保护，这不是该 API 文档明示的跨版本保证。SystemBackdropElement 在当前文档要求 Windows App SDK 2.0，本原型仍固定 1.8，采用整窗背景，不为该新控件升级依赖。Fluent 基准不等于通知中心内部动画参数，未发现能读取/订阅该内部时间线的公开契约。

[Windhawk PR #4986](https://github.com/ramensoftware/windhawk-mods/pull/4986) 讨论同进程多个 ControlCenterWindow 与 cloak，支持枚举全部候选，不能推导跨版本固定身份。
[样式实现](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/windows-11-notification-center-styler.wh.cpp) 与 [指南](https://github.com/ramensoftware/windows-11-notification-center-styling-guide/blob/main/README.md) 是修改系统界面的另一条路线，当前 demo 不注入且未复制源码。

必须实测：不激活 WinUI 点击是否保留系统面板；关闭关联什么状态；中英文身份提示是否可靠；普通置顶是否满足用户应用；多屏/DPI 与 Shell 重启是否稳定。

未找到足以保证此行为的公开通知中心状态契约。保留 Unknown、严格筛选、手动入口，测试结论只适用于记录的构建与场景。

用户日志实测：横幅与通知中心同为 ShellExperienceHost 的 Windows.UI.Core.CoreWindow；横幅未识别、UIA identity_bits=0，通知中心有标题/结构身份。PanelFollow 将发现和触发分开：未知窗口继续观察，只有已确认的面板身份才能触发，不用窗口类或长宽阈值猜测消息用途。类名与 UIA 特征仍非跨 Windows 版本的公开契约。
