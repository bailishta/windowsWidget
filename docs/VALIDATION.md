# 验证记录

验证日期：2026-09-20。环境：Windows 11 25H2 x64，Build 26200.9457；MSVC v143 14.44；Windows SDK 10.0.26100.0；固定 Windows App SDK 1.8.260804001。

## 已通过

- Release x64 全部工程编译通过：管理器、宿主、时钟、测试程序和测试插件。
- SDK 中的 Hello 示例独立编译通过，无 WinUI 或管理器源码依赖。
- 105 项自动化断言通过，使用真实 `WidgetHost.exe` 进程、命名管道、DLL 和 Job Object；功能测试的组件窗口保持隐藏。
- 覆盖配置往返、主题保存、上一份备份恢复、未来版本拒绝覆盖、无有效备份时只读保护、写入中断遗留临时文件。
- 覆盖无效清单、架构/SDK 不兼容、DLL 路径越界、损坏 DLL、ABI 拒绝、初始化崩溃、超时、加载中移除及缺失插件保留实例。
- 覆盖多个不同 PID、启停和重新启用、锁定和配置保存、正常退出回收宿主、管理器进程被终止后 Job Object 回收宿主、异常退出后恢复已提交实例。

并发测试：8 个真实宿主，每个测试插件初始化延迟 700 毫秒。

| 并发数 | 峰值启动实例数 | 总就绪时间 |
| --- | --- | --- |
| 1 | 1 | 6203 ms |
| 4 | 4 | 1672 ms |

本轮并发总耗时约为串行的 27.0%（约 3.71 倍加速）。此结果包含进程启动和通信开销，属于本机受控延迟测试，不代表任意第三方组件的固定加速比。测试也验证了组件逐个就绪，不等待整批完成。这不是编译器升级前后的性能对比。

## 实际界面检查

已在真实桌面启动管理器，观察到浅色与深色 Fluent 控件、Mica 背景、插件发现及添加时钟。时钟桌面宿主报告就绪耗时 78 ms；布局和显示器标识写入测试配置。检查中发现并修复 WinUI 主题资源元数据注册问题。

用户随后按 Esc 停止界面自动化，之后没有继续操作桌面。以下后续补丁已编译，但没有再次做视觉验收：标题栏深色属性、初始窗口按 DPI 放大、设置面板初始滚动位置。

## 损坏映像弹窗回归

早期损坏 DLL 用例触发过 Windows“损坏的映像”对话框。宿主现已在入口设置 `SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX`，让第三方 DLL 错误通过 IPC 返回。

新增检查已通过：损坏 DLL 在 3 秒以内返回 `Load plugin DLL` 错误，且不是依赖初始化超时终止。测试错误文件仅存在于 `out/Release/test-results` 的隔离目录，不用于内置时钟或用户插件。

## 时钟空白、桌面图标被矩形遮挡的修复

用户反馈旧版宿主显示“运行中”，组件区域却只显示壁纸并裁掉图标。只读窗口诊断发现，本机 `Progman` 的扩展样式为 `0x200080`，包含 `WS_EX_NOREDIRECTIONBITMAP`；其 `SHELLDLL_DefView` 使用层叠合成。旧版普通子窗口和 HWND Direct2D 渲染路径不适应该桌面合成结构。

此次宿主改用 `WS_EX_LAYERED` 与 Alpha 255 提供独立合成表面，时钟改用 Direct2D DC 渲染目标绘制到窗口 HDC，并记录绘制失败。没有修改 Explorer 样式。内置时钟版本为 1.0.1、随包修订号为 1；管理器启动时将用户目录中的旧内置时钟升级到带版本名的 DLL，保留布局、配置、旧 DLL 和清单备份。

新增非交互回归已通过：

- 隐藏的无重定向父窗口下，组件子窗口成功初始化不透明合成层。
- 加载真实时钟 DLL，通过 `WM_PRINTCLIENT` 绘制到离屏位图，验证深浅主题、12/24 小时配置与 96/144/192 DPI 下均存在背景和文字像素，没有渲染错误。此测试不显示窗口、不截取桌面。
- 旧内置时钟升级、完整 DLL 发布后切换清单、原文件及用户数据保留、重复安装跳过、较新修订不降级、其他插件不覆盖。

这些检查验证了绘制输出和升级行为，尚不能代替实际 Explorer 桌面的视觉验收。新包需要由用户从托盘退出旧版后启动，再检查时钟、图标及鼠标交互。

`WidgetTests.exe --diagnose-desktop` 只读输出相关窗口的父子关系、样式、可见性和位置，不修改 Explorer，也不创建桌面组件。

## 桌面网格与中英文（2026-09-20 后续修订）

- 网格测试通过：最近空位、避让图标与其他组件、连续分配、负坐标、缩放后的间距、桌面已满、组件过大。
- 真实桌面只读查询通过：本机 120 DPI，Shell 报告间距 93 × 103 像素、49 个图标；可以计算空位候选，但没有移动组件或图标。查询采用宿主相同的 STA 线程，MTA 不适用于本机的该 Shell 调用。
- 语言测试通过：只允许 `zh-CN` / `en-US`、创建时传递英语、运行时通过 IPC 改为中文、与既有实例一起保存和恢复。
- 加载真实时钟 DLL，在中英文、深浅主题、96/144/192 DPI 下检查背景和文字像素，12 种组合通过。
- `WidgetManager.exe --validate-ui --data-dir <隔离目录>` 在不激活窗口、不创建托盘的情况下构造真实 WinUI 控件，检查两种语言的标题和按钮。非 MSIX 进程使用 [Windows App SDK 的语言接口](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.windows.globalization.applicationlanguages.primarylanguageoverride?view=windows-app-sdk-1.8)。该测试不代表视觉排版验收。
- 本机安装的 VS 2022 Build Tools 是 17.14.39，MSVC 14.44.35207。保留工具链，Release 增加 `/GL` + `/LTCG`。相关机制见 [MSVC 全程序优化](https://learn.microsoft.com/en-us/cpp/build/reference/gl-whole-program-optimization?view=msvc-170)。未测得或宣称替换编译器后的加速比。
- 内置时钟升至 1.0.2 / 随包修订 2，复用文字格式资源、分钟未变化时跳过计时器重绘。同屏拖动避免重复重建绘制资源。

## 图标单元尺寸（2026-09-20 后续修订）

依据微软 [IFolderView::GetSpacing](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifolderview-getspacing) 对“当前图标与周围留白”的定义取格距，而非图标图像的固定像素尺寸。[GetSystemMetrics](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getsystemmetrics) 也区分 `SM_CXICON` / `SM_CYICON` 与排列单元 `SM_CXICONSPACING` / `SM_CYICONSPACING`。

- 新增格数计算测试、旧 DIP 配置迁移、运行中 1×1 调整、连续尺寸请求的旧消息过滤、实例 PID 保持、无效/小数输入拒绝、锁定、停用时修改、重启恢复。
- 真实时钟在普通与紧凑尺寸、中英文、深浅色、96/144/192 DPI 下的 24 组离屏像素检查通过。
- 隐藏 WinUI 验证通过：1×2、3×2 预设更新宽高 NumberBox；中英文控件均正常构造。
- 真实桌面只读核验：当前 120 DPI，格距 93×103 像素，3×2 计算为 279×206 像素；只计算了位置候选，没有移动任何窗口或图标。
- 内置时钟 1.0.3 / 修订 3 默认 3×2，支持 1×1 紧凑时间显示。已放置实例在旧配置迁移时按原尺寸就近换算，未强制全部重置为 3×2。

## 尚未验收（图标单元尺寸）

- 桌面组件本身的截图、鼠标拖动、边缘缩放、桌面图标不受影响。
- Win+D、普通窗口遮挡、托盘退出和 Explorer 重启后的恢复流程。
- 多显示器负坐标、100%/150%/200% 混合缩放、显示器热拔插。
- 其他 Windows 11 版本、ARM64、系统虚拟桌面的行为。

上述恢复与适配代码已经实现，但不能视为兼容性验收通过。没有重启用户的 Explorer、修改显示器缩放或系统主题。

用户已接手需要鼠标操作的测试，步骤见 [手动验收清单](MANUAL_TESTS.md)。

`WidgetTests.exe --desktop` 提供额外的桌面结构测试入口：检查组件可见性、父窗口、WS_CHILD、非置顶及不激活样式；它会短暂创建测试组件，本次在用户停止界面操作后未运行。
