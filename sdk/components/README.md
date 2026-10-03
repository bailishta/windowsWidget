# 自定义组件开发指南（供用户与 AI Agent 使用）

本文针对当前 Windows 11 Notification Companion 控制中心，包格式为 **schema 1**，原生接口为 **ABI 1**。目标是让你把一个独立组件交给 Agent 开发，生成可以直接导入的包，无需修改或重新编译主程序。

**给 Agent 的第一条指令：先读本文、当前 `WidgetSdk.h` 和 `widget.schema.json`，再确定组件类型。不要自行发明宿主 API。** 下文区分“实际限制”和“设计建议”；建议值并不是宿主已经实施的配额。

## 1. 先选组件类型

| 需求 | `kind` | 开发方式 |
| --- | --- | --- |
| 展示固定文字 | `text` | 只写 JSON；修改清单后重新扫描 |
| 可编辑、自动保存的便签 | `note` | 只写 JSON；直接在组件卡片内编辑 |
| 可添加、勾选、删除的清单 | `checklist` | 只写 JSON；宿主管理清单状态 |
| 点击打开网站 | `links` | 只写 JSON；只接受完整 HTTPS 链接 |
| API 数据、硬件信息、计算、自定义布局与业务交互 | `native` | 编写 x64 DLL，通过 C ABI 提供 XAML 或 Win32 子窗口 |
| 完全独立的组件应用、独立 WinUI 编辑界面 | `process` | 编写 x64 EXE，通过公开 PluginWire ABI 1 与管理器连接 |

零代码组件不能执行 JavaScript、Python、PowerShell 或 DLL。当前没有 HTML/WebView 组件协议、脚本入口、跨组件消息总线、权限声明系统或自动更新协议。需要这些业务能力时，选择原生 DLL 并在插件内部实现；不要通过新增 `kind` 或给 JSON 增加 `script` 绕过接口。

天气、待办、常用入口、系统状态现已是普通 `process` 组件包，可从组件库安装和卸载；这些业务名称不再是宿主特殊 kind。自制天气等组件可使用 `native` 或 `process`，无需修改主程序。

组件库提供便签、文字和蓝牙电量示例。“我的清单”和“开发入口”两个重复示例已移除；仍可导入自己设计的 `checklist` / `links` 包。待办与便签的编辑直接在卡片内完成，不再打开独立编辑窗口。待办支持新增、改名、删除和完成项过滤，便签在停止输入 500 毫秒后自动保存，结束编辑时立即保存。

## 2. 组件的职责与宿主的职责

宿主负责外框、组件名称与标题拖动区、Acrylic/主题、显示和隐藏、统一动画、启停、重启、顺序、摆放及系统区域避让。插件只负责内容区、业务数据、设置与自己的数据文件。

| 实际限制 | 当前行为 |
| --- | --- |
| 外框宽度 | 固定 320 DIP，清单里的 `width` 不会改变它 |
| 外框高度 | `height` 为 180–600 DIP，默认 240；建议使用整数 |
| 原生内容区 | 宽约 288 DIP；当前可用高度约为 `height - 72` DIP，最终以 `layout` 参数为准 |
| DPI | XAML 尺寸使用 DIP；`layout(width_px, height_px, dpi)` 传入物理像素，换算为 DIP 时乘以 `96 / dpi` |
| 显示时机 | 默认随通知中心或快速设置显示/隐藏；也可手动预览 |
| 空间不足 | 自动排列时部分卡片可能暂时隐藏；不能保证所有组件同时显示 |
| 运行方式 | 每个组件独立进程；隐藏卡片不等于停用组件 |
| 启动 | 20 秒内未就绪会被判定启动超时；`create` 应尽快返回并先显示加载状态 |
| 停止 | 宿主发送关闭请求，最多等待约 200 ms 后终止组件 Job；不能依赖退出时一定完成保存 |

不要创建自己的顶层常驻卡片来接管布局、修改任务栏/通知中心、争抢焦点或重做宿主动画。需要输入文字时，通过用户明确点击打开独立普通编辑窗口；卡片适合浏览、刷新和轻量操作。

**设计建议：** 一个组件解决一个明确需求；首屏优先显示最重要的信息和 1–3 个动作，长内容用滚动区域。使用主题资源、合理留白与文字截断；提供加载、空数据、失败、权限不足和过期数据状态。未知读数应显示未知，不能用演示值冒充真实结果。

## 3. 目录与包格式

每个组件一个子目录，根目录放 `widget.json`。原生包示例：

```text
plugins/
  README.md                 ← 本指南（发布/安装后提供）
  WidgetSdk.h               ← 当前接口头文件（发布/安装后提供）
  widget.schema.json        ← 清单 Schema（发布/安装后提供）
  user.my-widget/
    widget.json
    MyWidget.dll
    assets/                 ← 可选，只包含运行需要的资源
    dependencies/           ← 可选；嵌套依赖须由插件自行安排加载
    README.md               ← 组件自己的用途、构建和使用说明
```

源码仓库里的指南和 Schema 位于 `sdk/components/`，接口头文件位于 `sdk/WidgetSdk.h`。构建后的程序以及用户组件目录里，三份文件放在同一目录，供 Agent 本地读取。

程序启动时仅补齐用户组件目录缺失的指南、头文件和Schema，不覆盖已有文件。升级主程序后，如需更新本地开发资料，请先保留自己的修改，再从新版程序的 `plugins/` 目录同步这三份文件；接口头文件应与目标宿主版本一致。

默认用户组件目录是 `%LOCALAPPDATA%\WindowsWidget\CompanionDemo\plugins\`，可通过控制中心“打开组件目录”定位。`--data-dir` 会改变数据根目录，不要在 DLL 中硬编码默认路径。管理器只扫描组件目录的**直接子目录**中的 `widget.json`，放在根目录或多嵌套一层不会被发现。

开发源码、编译中间文件与待发布的包应分开。导入原生组件会复制所选 `widget.json` 所在目录的全部文件，不要选择仓库根目录里的清单。

### 清单公共字段与实际校验

| 字段/内容 | 要求 |
| --- | --- |
| 编码与大小 | JSON 使用 UTF-8，推荐不带 BOM；清单最多 64 KiB（65,536 字节） |
| `schema` | 必填，数字 `1` |
| `id` | 必填，全局唯一，1–64 字符；仅小写英文字母、数字、点、横线，不能以点开头或结尾 |
| ID 命名建议 | 使用 `user.<作者>.<用途>`；建议以字母/数字开头，以兼容 Schema；更新时保持 ID 不变 |
| Windows 保留名 | 第一个点之前不能是 `con`、`prn`、`aux`、`nul`、`com1`–`com9`、`lpt1`–`lpt9` |
| `name` | 必填，1–80 字符 |
| `description` | 可选，最多 300 字符 |
| `icon` | 可选，包内 PNG 的相对路径；文件必须已存在且最多 4 MiB，不允许绝对路径、冒号或 `.` / `..` 路径段 |
| `kind` | 必填，只使用上一节的五种自定义类型 |
| `height` | 可选，数字 180–600，默认 240；小数在运行时转成整数 |
| `text` | 可选字符串，最多 4000 字符 |
| `items` | 可选数组，最多 30 个初始条目 |
| `configuration` | 原生组件初始配置，使用 JSON 对象；插件自己定义和校验内部字段 |
| `entry` / `abi` | 原生组件必填；其他类型不要声明 |
| `dll` / `script` | 禁止出现，即使值为 `null` 也不允许 |

运行时文本长度按 Windows UTF-16 单元计算，部分 emoji 会占两个单元。Schema 用于编辑器提示和辅助检查；实际导入校验还会检查路径、文件存在性和保留名，因此通过 Schema 不代表一定能导入。未知字段不构成新能力：不要依赖宿主读取 `permissions`、`refreshInterval`、`version` 等未定义字段。

原生包通过“添加组件文件”导入时，文件总大小最多 **50 MiB（52,428,800 字节）**、最多 **256 个文件**，清单、DLL、资源和依赖都计入；包内不允许链接、目录联接等重解析点。直接放入目录后扫描不是同一条导入预检路径，开发与发布仍应遵守相同包约束。

## 4. 零代码组件：最快可用的路线

以下便签保存为 `user.my-note/widget.json` 即可导入：

```json
{
  "schema": 1,
  "id": "user.my-note",
  "name": "我的便签",
  "description": "随通知中心打开的个人便签",
  "kind": "note",
  "height": 280,
  "text": "今天最重要的三件事。"
}
```

- `text`：显示清单里的文字，内容修改后重新扫描。
- `note`：`text` 是初始内容；编辑后的内容保存在独立数据目录，修改清单不会覆盖已有便签。
- `checklist`：使用 `"items": ["第一件事", "第二件事"]`；每条初始字符串 1–200 字符。已有状态优先于清单初始值。清单的 30 条限制是导入初始值限制，当前编辑器最多保存 100 项。
- `links`：使用 `"items": [{"title": "示例网站", "url": "https://example.com/"}]`；标题 1–100 字符，URL 最多 2048 字符，必须以小写 `https://` 开头且有后续内容，不能含空格、双引号、回车、换行或制表符。点击交给系统默认浏览器；不支持本地程序路径、`file:`、`javascript:` 或自定义协议。

本地示例在源码的 `sdk/components/examples/`，发布版的 `examples/`。零代码导入只保存清单，不复制资源目录；图片、外部数据和自定义事件请使用原生路线。

图标是零代码资源的例外：声明 `"icon": "icon.png"` 时，导入会一并复制该 PNG，支持嵌套相对路径。图标用于控制中心列表与卡片标题；窗口图标由宿主按类型提供。不声明时使用天气、待办、系统状态、入口、便签、文字、清单、链接等对应的默认图标，通用原生组件使用主程序图标。无需为了换图标改宿主代码。

自制图标请参考微软的 [Windows 应用图标设计](https://learn.microsoft.com/en-us/windows/apps/design/iconography/app-icon-design) 与 [图标资源制作](https://learn.microsoft.com/en-us/windows/apps/design/iconography/app-icon-construction)。采用正视薄层叠放、简洁色块、克制渐变，检查透明背景下的深浅主题与 16/24/32/48 像素辨识度。仓库里的 `assets/icons/WINDOWS11_DESIGN.md`、`prompts.json` 和 `references/microsoft/` 包含官方出处、已下载参考和实际生成提示词；可以一起交给 Agent，避免仅写“Fluent 风格”导致过强的玻璃反光或膨胀立体效果。操作按钮继续使用系统 Fluent 符号。

## 5. 原生组件：自由编程与自定义界面

### 清单与构建

```json
{
  "schema": 1,
  "id": "user.my-widget",
  "name": "我的组件",
  "description": "由独立 DLL 提供数据和界面",
  "kind": "native",
  "abi": 1,
  "entry": "MyWidget.dll",
  "height": 300,
  "configuration": {"refresh_seconds": 60}
}
```

`entry` 必须指向已存在的包内相对路径，扩展名为小写 `.dll`；不允许绝对路径、冒号或 `.` / `..` 路径段。构建目标为 **Windows x64 DLL**，导出无名字修饰的 `WidgetGetApi`，入口签名为：

```cpp
WIDGET_EXPORT HRESULT __cdecl WidgetGetApi(uint32_t requested_version, WidgetApi* api);
```

使用当前 `WidgetSdk.h`；不要复制改造结构体或跨边界传递 STL、C++ 异常、WinRT 对象以及需要另一侧释放的内存。UTF-8 字符串以 NUL 结尾，调用期间借用的字符串如需留存要复制。宿主表和 `data_directory_utf8` 在 `destroy` 前有效；之后不能再访问或回调。

`WIDGET_ABI_VERSION` 仍为 **1**。`WidgetApi2` / `WidgetHostApi2` 表示通过 `size` 协商的扩展结构，**不是要求传入版本 2**。读取和写入扩展字段前必须检查结构大小，不能超出调用方提供的空间。

推荐 C++20、Visual Studio 2022 C++ Build Tools（v143）及仓库使用的 Windows SDK 10.0.26100.0。XAML 描述型 DLL 不需要自己初始化 WinUI；宿主提供 WinUI 运行时。把私有依赖放在主 DLL 同目录是最直接的做法。工作目录是宿主可执行文件目录，资源定位应基于 DLL 自身位置，不能依赖当前工作目录。

### 生命周期

| 回调/接口 | 插件应该做什么 |
| --- | --- |
| `WidgetGetApi` | 检查版本和结构大小，填写接口表；不进行耗时初始化 |
| `create` | 读取并校验初始配置、创建状态、返回实例与初始界面，启动必要的后台工作 |
| `layout` | 响应物理尺寸/DPI变化；HWND 路线更新绘制布局，XAML路线避免重复写死像素尺寸 |
| `theme` | 响应明暗变化；XAML优先使用继承的 `ThemeResource` |
| `ui_event` | 处理命名按钮点击和控制中心通用动作；尽快返回 |
| `configure` | 可实现配置应用；当前控制中心通过 `ui_event("settings")` 请求设置，不会自动调用它打开配置窗口 |
| `host.configuration_changed` | 保存完整的配置 JSON 对象；不是只传单个变更字段 |
| `host.log` | 输出诊断；当前 Companion 桥接到调试输出，不保证写入组件文件日志 |
| `destroy` | 标记停止，取消任务，解绑定时器/窗口，结束工作线程，释放全部资源 |

正常路径先协商接口、再 `create`，之后按需调用布局、主题和事件，最终销毁。`create` 失败时插件必须自行清理已分配资源；所有 ABI 回调应捕获自身异常，以 HRESULT 或可理解的状态报告错误。

### 界面路线 A：XAML 描述（推荐信息卡片）

声明 `WIDGET_CAPABILITY_XAML`，通过 `widget_host_v2(info->host)` 检查扩展宿主，调用 `host.ui_render(context, xaml_utf8)`。`create` 返回有效 `instance`，`content` 可为 `nullptr`。插件只提供完整 XAML 字符串，宿主创建和拥有控件。

```xml
<StackPanel xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
            xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
            Spacing="8">
  <Border Padding="12" CornerRadius="6"
          Background="{ThemeResource CardBackgroundFillColorDefaultBrush}">
    <TextBlock Text="组件已就绪" TextWrapping="Wrap" />
  </Border>
  <Button x:Name="refresh" Content="刷新" HorizontalAlignment="Stretch" />
</StackPanel>
```

实际限制与交互边界：

- 每次传入**单个根元素的完整 XAML**，UTF-8 字节数严格小于 128 KiB（131,072），末尾 NUL 不计入正文；`ui_render` 返回非零才表示成功，返回 0 必须报告渲染失败。
- 每次渲染会重新解析和替换整棵内容树，不是增量更新。只在状态改变时渲染；保存业务状态到实例中，不能依赖控件一直存在，也不能保证输入草稿或滚动位置自动保留。
- 宿主只把有 `x:Name` 的标准 `Button.Click` 转发为 `ui_event(instance, name)`。名称应唯一；控件模板里的按钮需要实际验证。
- `TextBox.TextChanged`、`CheckBox.Checked`、`Slider.ValueChanged`、选中值等不会作为数据回传；接口也不能查询这些控件当前值。仅加入这些控件的 XAML 并不能得到可用的输入功能。
- `ui_event("refresh")` 是控制中心“刷新组件”，`ui_event("settings")` 是控制中心“设置”。保留这两个名字并处理它们，其他动作使用例如 `openDetails`、`nextPage`；按钮点击和通用动作不带额外参数。
- XAML 不能用 `x:Class`、代码后置事件处理器或宿主未注册的自定义控件。动态字符串插入属性/文本前要转义 `& < > " '`；不要把网络返回的文本直接拼成 XAML。
- 不在 DLL 内创建 WinUI `Application`、XAML Island 或资源提供者；主题资源和控件由宿主提供。

### 界面路线 B：Win32 子 HWND（适合自绘与完整输入）

不声明 XAML capability，在 `create` 中返回本组件进程内的有效子窗口，`GetParent(content)` 必须等于 `info->parent`。插件自行处理窗口消息、绘制、输入、DPI和主题；宿主根据内容区尺寸定位该 HWND。不得返回其他进程窗口或自己的顶层窗口。

需要复杂表单、键盘输入或自定义图形时，可以选择这条路线，或从 XAML 按钮打开插件自己管理的普通 Win32 编辑窗口。ABI 1 没有向插件传入语言设置，也没有独立的显示/隐藏回调；自行设计语言配置，不要假设收到这些通知。

## 6. 后台任务、性能与数据保存

生命周期、布局、主题和事件回调运行在组件宿主的 UI 线程。网络、硬件扫描和耗时计算放在后台；**`ui_render` 只能在所属 UI 线程调用，跨线程会返回 0**。SDK没有提供调度器，插件可以在 UI 线程创建消息窗口，通过 `PostMessage` 或 UI 定时器消费后台结果；工作线程只更新受锁保护的快照，不操作 XAML。

**建议的实现流程：** `create` 先显示加载状态 → 后台查询 → 发布结果快照 → UI 线程渲染 → 下次刷新。合并重复刷新请求，避免并发重复扫描；提供超时、可取消任务、失败退避和缓存时间。网络信息可从 60 秒周期起步，硬件状态可从 30 秒起步，再按需求调整；这些不是宿主强制值。

宿主没有实施统一 CPU/内存/网络配额。卡片隐藏时进程仍可能运行，插件应使用合理刷新周期，避免忙轮询和频繁重建 XAML。停止时工作线程不能等待 UI 线程回调完成，防止 `destroy` 与后台线程互相等待。

| 数据 | 存放位置和规则 |
| --- | --- |
| 初始配置 | `widget.json.configuration`，只作为没有保存配置时的默认值 |
| 保存的配置 | 调用 `host.configuration_changed(context, 完整JSON对象)`，宿主写入 `data_directory_utf8/configuration.json`；下次 `create` 优先读取它 |
| 配置大小 | 保存文件应不超过 64 KiB；当前重启读取器也有此限制。保存回调无返回值，不能当成有确认的事务接口 |
| 业务状态/缓存 | 插件自行写入 `host.data_directory_utf8` 对应目录，例如 `state.json`、`cache.json` |
| 默认数据目录 | `%LOCALAPPDATA%\WindowsWidget\CompanionDemo\plugin-data\<id>\`；以宿主传入值为准 |

配置值要检查类型和范围，缺失字段使用默认值。业务状态添加自己的版本号，兼容旧数据并保留损坏文件用于排查。文件写入建议临时文件后原子替换，编辑或操作完成就保存，不要等到退出。发布包目录只放程序与资源，不在那里写用户状态，也不要修改其他组件数据或 `manager.json`。

DLL以用户权限执行普通原生代码；**独立进程是故障隔离，不是恶意代码沙箱**。清单没有权限审批或秘密存储功能。组件自己的 README 应说明访问的数据、网络服务、写入文件和依赖；凭据使用 Windows 凭据存储等合适方式，不要塞进待分享的包、示例配置或日志。

## 7. 交付、导入与更新

1. 在开发目录完成源码、构建脚本和自己的 README；生成独立发布目录，只放运行所需文件。
2. 检查 `widget.json`、唯一 ID、包大小、依赖架构和导出；原生包中的 DLL 必须先构建完成，才能导入。
3. 控制中心“添加组件文件”选择发布包里的 `widget.json`。自定义导入完成后默认停用，随后手动启用；也可直接安装组件库中的自定义示例。
4. 用 `Ctrl+Alt+W` 预览，再测试 `Win+N` / `Win+A` 的真实随行显示。通过控制中心检查设置、刷新、重启和停用。
5. 更新时保持 ID 不变，先停用，备份包和数据，替换已安装目录中的清单/DLL/资源，点击“重新扫描”后启用。重新扫描本身可能重启已启用组件，因此先停用再替换。

重复 ID 的导入不会覆盖已有组件；已存在同名目标目录也会阻止导入。当前没有内置覆盖升级和回滚按钮，需要自行备份和替换。卸载会停止组件进程，将完整安装包移到 removed-plugins 归档，保留 plugin-data。原安装目录腾空后可直接重新导入同一个 id，继续使用原有数据。所有组件均支持此流程。

用户修改后的便签、清单和保存的原生配置不会被新的默认值自动覆盖。需要迁移时由插件处理，而不是删除整个用户数据目录。

## 8. 可直接交给 Agent 的任务提示词

复制下面内容，替换方括号里的要求。把本指南、SDK和Schema的实际本地路径一起交给 Agent。

```text
请为 WindowsWidget Notification Companion 开发一个属于我的组件。
先完整读取组件目录的 README.md、WidgetSdk.h、widget.schema.json；
如果在源码仓库开发，分别读取 sdk/components/README.md、sdk/WidgetSdk.h、
sdk/components/widget.schema.json。以当前代码/头文件为接口依据。

需求：[想解决的问题、数据来源、主要信息、点击动作、需要的设置]
名称：[组件名称]
ID：[例如 user.alice.reading，不与现有组件重复]
类型：[让你按指南判断，或指定 native / note / checklist / links / text]
高度：[例如 300 DIP]
刷新与缓存：[例如每 60 秒，失败显示上次结果及更新时间]
隐私与外部访问：[允许访问的文件、硬件、API；凭据从哪里取得]

先确定类型和界面方案，再完成独立组件包。
只修改自己的开发目录，不为新业务修改或重编管理器，不新增宿主 API。
原生组件使用 x64 DLL 和 ABI 1，通过 size 检查扩展接口。
XAML路线只依赖命名Button点击，不假设能取回TextBox/CheckBox/Slider的值；
需要输入时采用普通编辑窗口或Win32子HWND。
不要阻塞UI线程；后台结果回到UI线程再ui_render；动态XAML转义文本。
实现加载、空数据、错误、刷新、设置、主题、持久化和资源清理。
默认值与保存状态分开，保留用户数据，不用演示值替代真实数据。

交付：可导入目录（widget.json + 必要DLL/资源）、完整源码、可重复执行的
PowerShell构建脚本、组件README、配置说明、验证结果与尚未验证的限制。
先自检和构建，再说明导入步骤；不自动启用未经我检查的新DLL。
如果本机缺少构建工具，明确报告，不把源码当成已构建成功的组件包。
```

## 9. 发布前验收清单

- [ ] 清单与当前 Schema 相符，ID 唯一，DLL存在且为 x64，`WidgetGetApi` 导出正确；包可从干净输出目录构建。
- [ ] 导入、启用、刷新、设置、停用与重启都可用；操作不会影响其他组件。
- [ ] 100% / 150% / 200% DPI，明暗主题，长中文与 emoji 都不溢出；内容区未侵入宿主标题区。
- [ ] 实际验证每个按钮；有输入功能时确认能取得并保存真实输入，不能只验证控件能显示。
- [ ] 无网络、超时、空结果、权限不足和错误配置有清楚状态；等待期间界面仍能操作。
- [ ] 重启保留配置和数据，更新不覆盖编辑状态，旧数据可迁移；快速停用不丢失已完成操作。
- [ ] 后台任务可取消，没有死锁、孤儿线程或子进程；日志不含凭据。
- [ ] `Ctrl+Alt+W` 预览及 `Win+N` / `Win+A` 真实显示/隐藏已检查，隐藏期间刷新频率合理。
- [ ] 组件 README 说明用途、外部访问、依赖、构建、安装、更新、数据位置和已知限制；未经执行的检查标为“未验证”。

## 10. 源码导航与参考示例

在仓库中开发时，Agent可按需读取：

| 路径 | 用途 |
| --- | --- |
| `sdk/WidgetSdk.h` | C ABI、结构大小协商、XAML能力与宿主回调 |
| `sdk/components/widget.schema.json` | 编辑器辅助校验 |
| `sdk/components/examples/` | 四种零代码组件完整清单 |
| `plugins/bluetooth-battery/` | 独立DLL完整源码、后台查询、UI线程渲染与构建脚本；复制时更换ID、窗口类名和输出目录 |
| `demo/NotificationCompanion/PluginCatalog.h` | 实际清单和路径校验 |
| `demo/NotificationCompanion/NativePlugin.h` | 实际DLL加载、XAML渲染和按钮事件桥接 |
| `demo/NotificationCompanion/ControlCenter.inc` | 导入、扫描、安装与管理行为 |
| `docs/SDK.md` / `docs/PLUGIN_DEV.md` | SDK说明与接入背景 |

参考插件的硬件算法、扫描周期和界面高度属于该插件业务，不是所有组件的规则。旧 `src/host`、旧示例和 `docs/archive/` 属于早期桌面宿主，不能据此假设当前随行组件支持旧交互或生命周期。


## 12. 独立 EXE 组件与统一装卸

所有类型都走同一安装流程。全新数据目录没有默认安装项；在组件库选择安装，或导入自己的 widget.json。手动导入默认停用，组件库安装会同时启用。卸载先停止组件 Job、等待进程释放文件，然后将完整包移出 plugins，归档到 removed-plugins。独立 plugin-data/<id> 不删除；重装同一个 id 可继续使用。空列表、只安装一个组件，以及全部卸载均受支持，主程序不会自动补回已卸载组件。

`process` 清单示例：

```json
{"schema":1,"id":"user.my-app","name":"我的组件","kind":"process","abi":1,"entry":"MyComponent.exe","height":264,"settings_label":"组件设置"}
```

entry 必须是包内存在的相对 EXE 路径。应用与私有依赖/资源随包一起复制，不允许目录联接或符号链接；process 包最多 256 MiB、2048 个文件，native 包仍为 50 MiB、256 个文件。可选 settings_label 最多 60 字符，只改变管理器设置按钮名称，不添加业务代码。

只负责显示的组件可在清单声明 `"has_settings": false`，主控制中心不显示该组件的设置按钮，也不向其发送设置命令。未声明时默认 true，其他组件维持原有设置入口。蓝牙设备电量示例使用 false。

process 通道定义见随指南分发的 PluginProcess.h（同时提供其 PluginCatalog.h 依赖）。子应用接收 `--plugin <清单绝对路径> --channel <共享映射名称> --data-dir <独立数据目录>`，以及可选 --en、--inspect。应用在自己的 UI 线程创建卡片，再填写 PluginWire 的 pid、panel、control，用 InterlockedExchange 发布 ready=1。panel 和 control 必须属于该进程。通道版本必须是 1，帧通过 PluginChannel::read 的序列锁读取。

管理器负责移动与统一动画。子应用读取 frame 的 work、reserved、target、dpi、automatic、dark、phase，更新主题和交互状态；不要自行执行进出场动画或持续搬动窗口。拖动仅在 automatic=0 时开启，结束时写 drag_position，递增 drag_revision 并清除 dragging。command=1 是打开设置或进入卡片内编辑，command=2 是兼容的刷新请求；使用 InterlockedExchange 取出命令。control 响应 WM_APP+101 关闭自身；发现父窗口失效也应退出。卡片默认使用无标题栏的 WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW。

当前 PluginWire 在 ABI 1 原有字段尾部增加可选 editing 标志，旧组件继续使用原有字段。采用当前头文件的进程组件可在卡片内编辑前通过 InterlockedExchange 设置 editing=1，使管理器保留当前卡片位置、系统区域避让和显示；等待 frame.phase=Visible 后移除该卡片 WS_EX_NOACTIVATE 并聚焦输入。完成编辑或切换到其他应用时保存数据、恢复非激活样式并清除 editing。该标志不会开启永久预览。标准待办与声明式便签实现了此流程，并接受 command=3 结束编辑；第三方组件不必实现该附加命令。

参考包源码位于 plugins/companion/weather、todo、quick、system，各自有独立的构建目标与入口文件，通过公共卡片运行框架复用 Acrylic、拖动和卡片内编辑行为。它们与第三方 process 包使用相同协议和验证，没有按 id 分派的主程序入口。添加新的 EXE 包无需修改或重新编译主程序。共享进程协议不是安全沙箱。
