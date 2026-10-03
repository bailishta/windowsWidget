# 插件开发指南

面向要写 WindowsWidget 第三方组件的人。ABI 参考在 [SDK 文档](SDK.md)，这里讲怎么把一个插件从零写出来、怎么调试、以及哪些坑一定会踩。

## 1. 十分钟跑通一个插件

插件就是一个导出 `WidgetGetApi` 的 x64 DLL，加一个 `widget.json`。最小可编译的例子在 `sdk/example/HelloWidget.cpp`（只依赖 `sdk/WidgetSdk.h` 和 Windows SDK，不引用任何管理器内部代码）。

```powershell
msbuild .\sdk\example\HelloWidget.vcxproj /p:Configuration=Release /p:Platform=x64
```

把生成的 `HelloWidget.dll` 和 `widget.json` 放进一个目录，再用开发宿主直接看效果——**不需要启动管理器**：

```powershell
# 目录布局：<某个目录>\hello\widget.json + HelloWidget.dll
.\out\Release\sdk\devhost\WidgetDevHost.exe <某个目录>
```

开发宿主是一个普通窗口，左边预览组件、下面显示插件通过 `host->log` 发出来的日志。改完 DLL 点“重新扫描”即可重载，不用重启管理器，也不会碰你的桌面。

## 2. 清单

`widget.json` 是插件目录里的唯一入口描述，管理器在**不加载 DLL** 的前提下校验它。任何一项不合法，插件会在管理器里显示为不可用并附上原因。

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `id` | 是 | 唯一标识；同一数据目录下重复会被拒绝 |
| `name` | 否 | 显示名，缺省用目录名 |
| `version` | 是 | 你自己的版本号，供用户辨认 |
| `sdk` | 是 | 必须为 `1` |
| `architecture` | 是 | 必须为 `"x64"` |
| `entry` | 是 | 插件目录内的 DLL 相对路径；绝对路径、含 `..`、或最终落在目录外都会被拒绝 |
| `gridColumns` / `gridRows` | 否 | 默认宽高格数，**必须成对**，各取 1～12 的整数 |
| `width` / `height` | 否 | 旧版 DIP 默认尺寸，缺省 260×160；未给格数时按它在首次放置时换算 |
| `minWidth` / `minHeight` | 否 | DIP 下限，缺省 140×100，不得小于 32 |
| `maxWidth` / `maxHeight` | 否 | DIP 上限，缺省 1000×1000，不得大于 4096 |

`width` / `height` 必须落在 `[min, max]` 内，否则清单被判为非法。

一个完整例子：

```json
{
  "id": "example.hello",
  "name": "Hello 示例",
  "version": "1.0.0",
  "sdk": 1,
  "architecture": "x64",
  "entry": "HelloWidget.dll",
  "gridColumns": 3,
  "gridRows": 2,
  "width": 240,
  "height": 140,
  "minWidth": 140,
  "minHeight": 100,
  "maxWidth": 640,
  "maxHeight": 480
}
```

**尺寸怎么理解。** 一个“格”是当前桌面图标排列单元，含图标周围的留白，宽高独立、由系统给出（本机 120 DPI 实测为 93×103 像素，所以 3×2 就是 279×206）。组件外框正好是列数×格宽、行数×格高。你的 `minWidth`/`maxWidth` 以 DIP 限制外框，尺寸越界会在管理器里报错而不是被默默截断。别把 `gridColumns` 定得让换算结果超出自己的 `maxWidth`——那种插件永远放不下。

## 3. ABI 契约

导出入口：

```cpp
extern "C" __declspec(dllexport) HRESULT __cdecl WidgetGetApi(uint32_t version, WidgetApi *api);
```

宿主先填好 `api->size = sizeof(WidgetApi)`、`api->version = WIDGET_ABI_VERSION` 再调用你。你要检查版本与结构大小，然后填满五个回调并返回 `S_OK`。宿主随后还会校验：`version == 1`、`size >= sizeof(WidgetApi)`、且五个函数指针**全部非空**——首版 ABI 没有可选回调，缺一个整个插件就会被判为不兼容。

| 回调 | 契约 |
| --- | --- |
| `create(info, instance, content)` | 创建**本进程内**、以 `info->parent` 为父窗口的 `WS_CHILD` 子窗口；失败返回错误 HRESULT 并把两个输出置空。宿主会强校验子窗口的父窗口与本进程身份，不符合就终止该实例 |
| `destroy(instance)` | 释放你自己分配的一切。它**不是每次都会执行**（见下） |
| `configure(instance, json)` | 应用新配置；成功 `S_OK`，无效配置返回失败 HRESULT |
| `layout(instance, width_px, height_px, dpi)` | 宿主已经调整好你的子窗口大小，通知可用像素区域与有效 DPI，在这里重建绘制资源 |
| `theme(instance, dark)` | `1` 深色、`0` 浅色，收到后重绘 |

**四条必须遵守的规则**（违反任意一条都会以难查的方式失败）：

1. **所有回调都在宿主的 UI 线程上执行。** 不要把网络、磁盘、耗时计算放在回调里——那会卡住整个组件。放到你自己的工作线程，算完用 `PostMessage` 把结果送回你的子窗口。工作线程里**不要**直接操作 HWND，也不要调用服务表。
2. **传入的 UTF-8 字符串只在本次调用期间有效**（唯一的例外是 `data_directory_utf8`，它活到 `destroy`）。要留着就自己复制。
3. **不要把 C++ 异常抛出 ABI 边界**，也不要跨 DLL 传 STL 容器、或把在一个模块里分配的内存交给另一个模块释放。每个回调自己 `try/catch` 并转成 HRESULT。
4. **`DllMain` 只做轻量初始化。** 别在里面启动线程、读文件、发网络请求。

初次的 `create` 必须在 **15 秒**内完成，否则会被判为初始化超时。

### `destroy` 不保证被调用

管理器用 Job Object（`KILL_ON_JOB_CLOSE`）托管宿主进程。管理器崩溃或被强制结束时，宿主是被直接终止的，`destroy` 不会跑。正常退出时留给宿主的优雅时间也只有约 1.5 秒。

**结论：任何重要数据都要在变更时立刻提交，不要攒到 `destroy` 再写盘。**

## 4. 宿主提供的服务

`WidgetCreateInfo.host` 指向这张表：

| 成员 | 说明 |
| --- | --- |
| `log(context, level, utf8)` | 写入 `manager.log`，`0=DEBUG`、`1=INFO`、`2=WARN`、`3=ERROR`。管理器会保留级别并附上实例 ID、宿主 PID 和调用线程 ID。默认 INFO 过滤掉 DEBUG，用 `WidgetManager.exe --log-level debug` 启动可看到 |
| `configuration_changed(context, json_utf8)` | 插件内部操作改变了设置时通知管理器保存。参数必须是 JSON 对象。时钟的右键切换 12/24 小时制就是走这条 |
| `data_directory_utf8` | 每个实例独立的数据目录，由宿主创建，生命周期持续到 `destroy` |

`data_directory_utf8` 指向 `<数据根>\data\<实例 GUID>`（默认数据根是 `%LOCALAPPDATA%\WindowsWidget`）。**注意：在管理器里移除实例不会删除这个目录**，而实例 ID 是新的 GUID，所以旧目录以后不会再被访问。自己控制写入量，别把缓存无限往里堆。

`log` 是唯一的日志通道，内容会被截断到 8192 字节、换行转义成单行。不要往里写配置正文或用户隐私数据。

## 5. 绘制

宿主给你的是容器里的一个子窗口，内容区尺寸 = 容器客户区减去顶部 24 DIP 的拖动栏和四周 5 DIP 衬垫（宿主按 DPI 换算后调用 `layout` 告诉你确切数值）。

**用 `BeginPaint` 拿到的 HDC 绘制。** 桌面在 Windows 11 上使用了 `WS_EX_NOREDIRECTIONBITMAP`，宿主容器是跨进程的层叠窗口，直接绑定 HWND 的 DXGI 交换链在这种合成结构下不可靠——时钟插件因此用 `ID2D1DCRenderTarget` 做软件渲染，这是推荐做法：

```cpp
auto properties = D2D1::RenderTargetProperties(
    D2D1_RENDER_TARGET_TYPE_SOFTWARE,
    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
factory->CreateDCRenderTarget(&properties, &target);
target->BindDC(dc, &rect);            // dc 来自 BeginPaint
target->SetDpi(dpi, dpi);             // 用 layout 给的 dpi，不要假设
```

`layout` 传进来的 DPI 才是所在显示器的有效 DPI，**不要**假设 Explorer 父窗口的 DPI 就是你的显示器 DPI。

### 处理 `WM_PRINTCLIENT`

除了 `WM_PAINT`，请把绘制逻辑也挂到 `WM_PRINTCLIENT` 上（收到时用传入的 HDC 画同一份内容）。开发宿主的 `--shot` 离屏截图、以及仓库里的像素回归测试都依赖它；只处理 `WM_PAINT` 的插件截出来是整幅空白。时钟插件是现成的写法参考：

```cpp
if (m == WM_PRINTCLIENT) { self->paint(reinterpret_cast<HDC>(w)); return 0; }
if (m == WM_PAINT)       { self->draw(); return 0; }
```

## 6. 用开发宿主调试

```powershell
WidgetDevHost.exe <插件目录> [选项]

  --id <插件id>      选择插件，默认取第一个可用的
  --shot <文件.bmp>  离屏渲染后写出 BMP 并退出（不显示窗口）
  --config <json>    传给插件的初始配置，默认 {}
  --data <目录>      插件的实例数据目录，默认 <exe>\devdata
  --dark | --light   起始主题，默认深色
  --zh | --en        起始线程 UI 语言，默认中文
  --no-layered       不启用与生产一致的 WS_EX_LAYERED 合成表面
  --quiet            不向控制台输出，只写界面日志
```

开发宿主刻意**不碰**你的桌面：不枚举桌面图标、不做网格吸附、不占用摆放互斥锁、不走命名管道。它复用的是同一套清单校验、同一套回调顺序、同一套内容区尺寸计算和同一套合成表面，所以预览与桌面上的表现一致。

界面上的按钮分别做这些事：切换预设大小、深浅色、中英文（会按契约再调用一次 `theme` 让插件刷新）、重建（销毁插件与容器后重新加载，与 Explorer 重启时的恢复路径一致）、截图、重新扫描（改完 DLL 后重载）。配置框里改完 JSON 点“应用配置”会走 `configure`。

开发宿主**故意不设置** `SetErrorMode`，所以插件崩溃会正常弹出调试对话框——生产宿主则会把这类错误转成 IPC 错误。要抓崩溃现场就附加调试器，或配置 WER 本地转储（`HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps`）。

### 离屏渲染与脚本化

```powershell
WidgetDevHost.exe .\plugins --shot out.bmp --dark
```

以退出码 0 表示成功。整幅同色会在日志里给出提示，通常就是上面说的没处理 `WM_PRINTCLIENT`。仓库的测试套件正是用这条路径做的插件渲染回归，你可以照搬进自己的 CI。

### 附加到生产宿主

`WidgetHost.exe` 必须由管理器经命名管道启动（不给 `--pipe` 会直接返回 2），所以不能在 VS 里直接 F5。做法是：先跑起管理器并添加你的组件，然后 **调试 → 附加到进程**，选 `WidgetHost.exe`、代码类型设为“本机”。装 Microsoft Child Process Debugging Power Tool 可以让 VS 自动附加到管理器新建的子进程。

日志：`%LOCALAPPDATA%\WindowsWidget\logs\manager.log`（用 `--data-dir` 时在指定目录下），要点见 [日志说明](LOGGING.md)。

## 7. 为插件写测试

不需要真实桌面。仓库里 `tests/main.cpp` 的 `clock_render_test` 是一套现成的夹具：建一个隐藏的父窗口 → 给子窗口启用合成表面 → `LoadLibrary` 你的 DLL → 自建 `WidgetHostApi` 与 `WidgetCreateInfo` → 调用 `create` → 逐个组合调用 `layout`/`theme`/`configure` → 用 `WM_PRINTCLIENT` 画到 `CreateDIBSection` 的 32bpp 缓冲 → 直接统计像素并断言。

要注意两点：计时器和“工作线程 → UI 线程”这条路径只有在有消息循环时才会跑，所以测试进程要自己泵消息；`create`/`layout`/`theme`/`configure` 必须与消息循环在同一个线程上调用（ABI 就是这么约定的）。

## 8. 上线前的自查

| 症状 | 常见原因 |
| --- | --- |
| 管理器里插件显示为不可用 | 清单被拒：`entry` 越界、`sdk` 不是 1、`architecture` 不是 x64、`width` 不在 `[min,max]`、重复 `id`、DLL 不存在 |
| 组件显示“加载失败” | `WidgetGetApi` 返回失败、回填的 `WidgetApi` 不完整、`create` 返回错误 HRESULT |
| 组件位置是“桌面暂不可用” | 找不到空闲格位；腾出空间后会自动恢复，不必手动重试 |
| 界面空白但宿主报告运行中 | 绘制资源没在 `layout` 里按传入 DPI 重建；或用了对层叠父窗口不可靠的交换链 |
| `--shot` 截出来是整幅同色 | 没有处理 `WM_PRINTCLIENT` |
| 组件卡住整个桌面 | 在回调里做了耗时工作；把网络/磁盘挪到工作线程，用 `PostMessage` 回 UI 线程 |
| 插件崩溃后实例停在 Error | 崩溃会结束整个宿主进程，这是设计如此；修好插件后在管理器里点“重试” |

## 9. 目前做不到的事

- **窗口透明度与模糊。** 组件是 Explorer 桌面下的子窗口，Windows 11 的系统亚克力（`DWMWA_SYSTEMBACKDROP_TYPE`、`DesktopAcrylicController`）与 `SetWindowCompositionAttribute` 亚克力**只对顶层窗口生效**，在这个位置拿不到。宿主容器目前固定为不透明合成表面（Alpha 255）。
- **文字输入。** 容器是 `WS_EX_NOACTIVATE` 且对 `WM_MOUSEACTIVATE` 返回 `MA_NOACTIVATE`，组件永远不会成为活动窗口，键盘消息进不来——这与“组件不抢焦点、不打断你正在做的事”的定位是同一个决定的两面。需要输入的插件（例如待办）应当在点击时弹出自己的顶层编辑窗口，而不是试图让组件本身获得焦点。
- **通知/可见性/网络状态回调。** ABI v1 没有这些通道。组件被宿主隐藏时插件收不到通知，网络与电源状态要自己监听。

这些是已知边界，不是配置问题；需要时请一并讨论 ABI 演进方案（在 `WidgetHostApi` 尾部追加字段并用 `size` 判定，或按 COM 的 `query_interface` + IID 模式引入可选能力）。
