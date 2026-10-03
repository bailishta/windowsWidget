# 改动总结：让组件跑在 WinUI 3 上

本文记录本轮工作的背景、实测结论、架构改动、新增组件、修复清单与验证结果，以及尚未验证和需要决策的部分。

---

## 一、目标与结论

**目标**：主程序与组件都用 WinUI 3；在此基础上提供日历、待办、天气三个常用组件。

**结论**：做到了，但代价明确——

- 组件**不能再是 Explorer 桌面的子窗口**，必须是顶层窗口。这是 WinUI 3 的硬性前提，不是实现选择。
- 因此组件的 z-order 只能"尽量贴到桌面层"，**无法 100% 复刻桌面子窗口的层次**。
- 每个组件进程的内存成本从 D2D 自绘的约 10–15 MB 上升到 **81 MB**（实测，稳定无增长）。

---

## 二、三条硬约束（均为实测，不是推测）

### 1. 插件 DLL 里创建不了 WinUI 对象

在插件 DLL 中构造 `DesktopWindowXamlSource` 或任何 WinUI 控件，进程直接 fail-fast。gdb 栈：

```
Microsoft.ui.xaml!DlgGetActivationFactory
  → ModernResourceProvider::Create 返回 E_INVALIDARG
  → RoFailFastWithErrorContextInternal2
```

同样的代码写在宿主 exe 里完全正常。原因是 WinUI 按"激活模块"创建资源提供者，从被加载的 DLL 里做走不通。

### 2. XAML 岛不能挂在跨进程父链的窗口上

```
容器 SetParent 到 Explorer 桌面   → island.Initialize 失败：E_ACCESSDENIED
容器是顶层窗口 / 自有窗口的子窗口 → 成功
```

这是用 exe 测的，所以与"谁创建"无关，是窗口位置决定的。**桌面挂接与 WinUI 3 互斥。**

### 3. 元凶：`SetDefaultDllDirectories`

宿主原先在加载插件前调用 `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`。它会**限制整个进程的默认 DLL 搜索路径**，导致 WinUI 之后按默认路径查找模块与资源失败，最终在 XAML 运行时里 fail-fast。

这一条解释了此前所有自相矛盾的现象（"先建岛就成功、加载插件就崩"）——因为那个调用正好紧跟在加载插件之前。**已从生产宿主与开发宿主中移除**；`LoadLibraryExW` 上的搜索标志保持不变，插件目录优先、默认目录兜底的搜索策略没有降低安全性。

---

## 三、架构变更

### 组件窗口：桌面子窗口 → 顶层窗口

| | 改前 | 改后 |
|---|---|---|
| 窗口 | `SetParent` 到 Progman/WorkerW，`WS_CHILD` | 顶层窗口，`WS_EX_TOOLWINDOW \| WS_EX_NOACTIVATE \| WS_EX_LAYERED` |
| 坐标 | 映射到桌面父窗口客户区 | 屏幕坐标 |
| 网格吸附 / 图标避让 | 保留 | 保留 |
| 兄弟组件识别 | `EnumChildWindows` 桌面子窗口 | `EnumWindows` 顶层窗口（按类名 + 窗口属性） |

### z-order 策略

组件是顶层窗口后，必须主动维持层级。策略是**把组件插到"桌面窗口正上方"**：

```cpp
// SetWindowPos(W, X) 把 W 放到 X 下面；插到"当前位于桌面之上的那个窗口"的下方，
// 组件就正好落在壁纸之上、所有普通窗口之下。
HWND anchor = GetWindow(desktop, GW_HWNDPREV);
SetWindowPos(window, anchor ? anchor : HWND_BOTTOM, ...);
```

在 `place()` 与每 1 秒的巡检里重新执行一次。

**曾经的错误做法**：贴到"当前前台窗口"下面，并在锚点失效时回退 `HWND_TOP`。实测表现为：打开文件夹时组件全部跑到最顶层。该回退已删除。

### 宿主托管 XAML

插件 DLL 碰不了 WinRT，所以界面由宿主建立、插件只做描述：

```
插件 DLL（纯 Win32）─── ui_render(完整 XAML 文档) ───▶ 宿主（持有 XAML 岛）
插件 DLL        ◀────── ui_event(命名按钮的元素名) ─── 宿主
```

宿主在解析 XAML 后，遍历对象图给每个**有名字的 `ButtonBase`** 挂 Click 处理，按元素名回调插件。插件因此完全不接触 WinRT，也拿不到会破坏 UI 线程的对象图。

---

## 四、ABI v2（纯追加，旧插件不受影响）

`WidgetHostApi2` 与 `WidgetApi2` 保持与 v1 相同的前缀布局，新增字段通过 `size` 字段守卫，与 `STARTUPINFOEX` 同一套做法。

| 新增 | 方向 | 说明 |
|---|---|---|
| `WidgetApi2.capabilities` | 插件 → 宿主 | `WIDGET_CAPABILITY_XAML`：界面由宿主的 XAML 岛呈现 |
| `WidgetApi2.ui_event(instance, element)` | 宿主 → 插件 | XAML 中命名按钮被点击 |
| `WidgetHostApi2.ui_render(context, xaml)` | 插件 → 宿主 | 交出完整 XAML 文档；返回 0 表示被拒绝 |

宿主以 `sizeof(WidgetApi2)` 作为 `api.size` 的初值；v1 插件会用自己的小尺寸覆盖，宿主只校验 `>= sizeof(WidgetApi)`，因此**旧插件继续可用**（时钟插件实测仍正常加载与创建）。

---

## 五、新增组件

三个组件都是**真 WinUI 3 控件**（`Button`、`TextBlock`、`ScrollViewer`、`StackPanel`、`Grid`），不是自绘。

### 日历 `plugins/calendar`

月视图、星期表头、今天用强调色圆点高亮；点 ‹ › 换月，点标题回到本月。字号随组件宽度自适应，小于阈值时只留月份标题。每 30 秒检查一次日期是否跨天，跨天才重绘。

### 待办 `plugins/todo`

完成/未完成计数、逐项勾选与删除、可滚动列表。

- **数据位置**：清单是业务数据，存在插件自己的实例数据目录（`<数据目录>/todos.txt`），不写进管理器的 `settings.json`。写入采用"临时文件 + `MoveFileEx` 替换"，因为组件随时可能被 Job Object 杀掉。
- **新增条目**：组件容器是 `WS_EX_NOACTIVATE`，拿不到键盘焦点，无法在组件里打字。因此**配置里的 `items` 数组是收件箱**——在管理器的插件配置里填 `{"items":["买牛奶"]}` 并应用，插件把它并入自己的清单，然后清空收件箱。勾选与删除在组件内用鼠标完成。
- 格式是插件私有的单行文本（`1<TAB>内容`），因为数据永远不离开插件自己的目录，手写读写可以避免引入任何 JSON 依赖。

### 天气 `plugins/weather`

城市、当前温度、天气描述、最高/最低、更新时间；点 ⟳ 手动刷新，每 30 分钟自动刷新。

- **数据源**：Open-Meteo，无需 API key。
- **网络**：WinHTTP 在**工作线程**执行，完成后 `PostMessage` 回 UI 线程再更新界面（SDK 契约要求所有宿主服务调用都在 UI 线程）。同一时刻只允许一个请求在飞。
- **离线**：最后一次成功的响应缓存在实例数据目录，取数失败时显示缓存并标注"离线，显示上次结果"。
- **线程生命周期**：`destroy` 里 `join`，不 detach——移除组件后不留下孤儿线程。
- 配置：`{"location":"北京","latitude":39.9,"longitude":116.4}`。

---

## 六、修复清单

| # | 问题 | 根因 | 位置 |
|---|---|---|---|
| 1 | 加载插件后任何 WinUI 调用 fail-fast | `SetDefaultDllDirectories` 限制进程默认搜索路径 | `src/host/main.cpp`、`sdk/devhost/WidgetDevHost.cpp` |
| 2 | 打开文件夹时组件跑到最顶层 | z-order 兜底分支使用 `HWND_TOP`；且锚点选错（前台窗口） | `src/host/Desktop.h` |
| 3 | 天气显示 `2026°` 与"未知" | Open-Meteo 的 `"temperature_2m"` 首次出现在 `current_units` 段（字符串值 `"°C"`），旧扫描从那里往后抓到了 `"time":"2026-09-20"` 的年份 | `plugins/weather/Weather.cpp` |
| 4 | 天气出现 `□` 方块 | 源码中的图标字体私有区码位在写入时丢失，成为无效字符 | `plugins/weather/Weather.cpp`（改用 Segoe UI 通用符号 ☀ ⛅ ☁ ❄ ⚡ ☂） |
| 5 | 插件 DLL 携带 `app.manifest` 与 `windowsapp.lib` | 插件不是应用程序，且不应引入 WinRT | `plugins/*/*.vcxproj`（改用普通 Win32 依赖列表） |

---

## 七、文件清单

### 新增

| 文件 | 行数 | 说明 |
|---|---|---|
| `sdk/WidgetSdk.h`（改写） | 99 | 增加 v2 结构体与能力位 |
| `src/host/XamlSurface.h` | 220 | 宿主侧 XAML 岛托管：进程级 XAML 启动、解析、按钮事件、尺寸/主题、内容去重 |
| `plugins/calendar/` | 402 | 日历组件（.cpp / .vcxproj / widget.json） |
| `plugins/todo/` | 463 | 待办组件 |
| `plugins/weather/` | 604 | 天气组件 |
| `build/WinUI.props` `.targets` | 92 | Windows App SDK / WinUI 接线，宿主与开发宿主共用 |
| `sdk/devhost/` | 853 | 独立开发宿主（插件作者不用启动管理器就能调试） |
| `docs/PLUGIN_DEV.md` | 196 | 插件开发指南 |
| `docs/LOGGING.md`、`src/common/Logger.h` | — | 异步日志系统（本轮之前已存在的工作，随本次一并提交） |
| `sdk/spike/xaml-island/` | 520 | 技术验证工具，记录"岛能否可用"的实测；**不是产品代码，可不分发** |

### 修改

`README.md`、`WindowsWidget.proj`（新增 4 个工程）、`build.ps1`（插件资源整理）、`package.ps1`、
`docs/SDK.md`、`docs/VALIDATION.md`、`docs/MANUAL_TESTS.md`、`sdk/example/widget.json`、
`src/host/Desktop.h`、`src/host/DesktopGrid.h`、`src/host/main.cpp`、`src/host/WidgetHost.vcxproj`（WinUI 接线 + `ContentPreTranslateMessage`）、
`src/common/Common.h`、`src/common/Engine.h`、`src/common/Model.h`、`src/manager/main.cpp`、`src/manager/MainWindow.xaml`、`tests/main.cpp`、`tests/TestWidget.cpp`

---

## 八、验证结果

| 项 | 结果 |
|---|---|
| 编译 | 10 个工程 Release x64 全部通过 |
| 自动化断言 | **130 项全部通过**（含 6 项开发宿主断言） |
| 三个 XAML 组件 | 均在开发宿主中实测渲染成功（天气渲染 2 次 = 取数完成并回主线程更新） |
| 旧 ABI 兼容 | 时钟插件（v1 ABI）仍正常加载并创建 |
| 天气解析 | 用真实响应核对：修复后得 23.7°C / code=3，与标准 JSON 库一致 |
| 内存 | 单组件进程工作集 **81 MB，20 秒零增长**（无泄漏） |

### 内存防护措施

- 宿主对**内容相同的 XAML 文档直接跳过**，不重新解析、不换树。
- 切换界面时**先释放旧树再挂新树**，避免两棵 XAML 树同时存在。
- 插件侧都有变更检测：定时器到期但内容没变就不生成文档（日历只在跨天重绘，天气只在取数完成时重绘，待办只在增删改时重绘）。

---

## 九、已知限制与未验证项

### 结构性限制

1. **z-order 是补丁，不是真正的桌面图层。** 能实现"普通窗口覆盖组件"，但在点桌面空白、Win+D、Explorer 重启、显示器切换等场景下的行为与真正的桌面子窗口不同，需要实测确认可接受程度。
2. **组件不再随 Explorer 重启而销毁重建**，`WM_DESTROY` 恢复路径的触发条件随之改变，相关回归测试需要重新审视。
3. **内存成本**：每个组件一个进程且加载 WinUI 运行时，约 81 MB。四个组件约 320 MB。
4. **待办无法在组件内输入文字**（容器不可激活，拿不到键盘焦点），新增条目必须经由管理器的插件配置。

### 未验证

- 真实桌面上的**视觉与交互**（拖动、缩放、点按钮、被覆盖、Win+D）——全部未做视觉验收。
- **管理器路径**：三个组件只在开发宿主中验证过；生产宿主 `WidgetHost.exe` 是同一套代码，但没有实际通过管理器跑过。
- 多显示器、混合 DPI、显示器热拔插。
- 长周期运行的内存表现（只测了 20 秒）。

---

## 十、与需求文档的偏离（需要确认）

原始需求文档（`C:\Users\17999\Desktop\新建 文本文档.md`）中以下两条已被本轮改动实质改变：

| 文档原文 | 现状 | 原因 |
|---|---|---|
| 「组件位于桌面层，普通应用窗口可以覆盖」 | 组件是顶层窗口，靠 z-order 维持层级 | WinUI 3 的硬性前提（见第二节第 2 条） |
| 「挂接失败时……**不自动改成普通窗口**」 | 已经整体改成了普通（顶层）窗口 | 同上 |

文档另一条「组件：独立 DLL，由作者创建 Win32 窗口，**不要求依赖 WinUI 3**」仍然成立且被强化了——**插件现在完全不依赖 WinUI，甚至不依赖 WinRT**，WinUI 只存在于宿主侧。

其余（管理器、单宿主单实例、命名管道、Job Object、并发调度、配置持久化、故障隔离）与文档一致。

---

## 十一、如果这个代价不可接受

回退路径清楚：把组件改回桌面子窗口（`Desktop::attach` 恢复 `SetParent`），插件改用 Direct2D 自绘。届时：

- z-order 恢复天然正确，内存回落到 10–15 MB/组件；
- 失去真 WinUI 3 控件与将来的系统亚克力。

两条路的插件结构都已明确，切换成本主要在宿主与三个插件的渲染层。
