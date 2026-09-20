# 小组件 SDK v1

SDK 采用 C ABI，组件用 C++ 实现并导出 `WidgetGetApi`。一个插件目录包含 `widget.json`、x64 DLL 及可选资源。运行程序不需要重新编译即可发现插件。

## 最小示例

`sdk/example` 是独立 MSBuild 工程，仅依赖 `sdk/WidgetSdk.h` 和 Windows SDK。在 VS 2022 的 x64 Native Tools 命令环境执行：

```powershell
msbuild .\sdk\example\HelloWidget.vcxproj /p:Configuration=Release /p:Platform=x64
```

把生成的 `sdk/example/bin` 中 `HelloWidget.dll` 和 `widget.json` 放入 `%LOCALAPPDATA%\WindowsWidget\plugins\hello`，在管理器中重新扫描。SDK 目录可以整体复制到其他项目；示例不引用管理器内部代码或 NuGet 包。

## 清单

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

`gridColumns` / `gridRows` 指定默认宽高格数，必须一起提供，取 1～12 的整数。每格采用当前桌面 `IFolderView::GetSpacing` 返回的图标排列单元，包含周围留白；组件外框（含拖动栏）宽高分别是列数、行数乘以单元宽高。未提供这两个字段的旧插件根据 `width` / `height` 在首次放置时换算。

`width` / `height` 是兼容旧版本的默认 DIP 尺寸；`minWidth` / `minHeight` / `maxWidth` / `maxHeight` 继续以 DIP 限制实际外框，最小不能小于 32，最大不能超过 4096，默认值须位于范围内。网格尺寸超出插件限制时报告错误，插件作者应选择合适的限制以支持小尺寸。`entry` 是插件目录内的相对路径。重复 ID、架构不匹配、SDK 不兼容、清单错误或 DLL 缺失会显示为不可用。

## 生命周期和线程

1. 宿主安全加载 DLL，查找 `WidgetGetApi`，传入 ABI 版本及预设 `size` 的 `WidgetApi`。
2. 插件检查版本和结构大小，填入函数指针，成功返回 `S_OK`。
3. `create` 接收父 HWND、DPI、主题、JSON 配置和宿主服务表，返回插件对象及本进程内的子 HWND。窗口必须以传入 HWND 为父窗口。
4. 宿主管理外层窗口和消息循环，调用 `layout`、`theme`、`configure`。
5. `destroy` 释放对象、窗口、线程、计时器和插件自己的资源。

所有插件入口回调都在宿主 UI 线程执行，宿主服务也仅允许在该线程调用。网络/磁盘/耗时计算应放到插件工作线程，结果通过自己的窗口消息回到 UI 线程；禁止让插件工作线程直接操作 HWND 或调用服务表。`DllMain` 中只做必要的轻量初始化，不启动耗时任务。

初次初始化须在 15 秒内完成。插件异常崩溃或错误会终止当前宿主，不影响其他实例。Explorer 重启时可能对同一个实例执行销毁和重新创建，因此业务数据不要只留在 UI 对象中。

## API 契约

| 函数 | 契约 |
| --- | --- |
| `create(info, instance, content)` | 创建属于本进程的 `WS_CHILD` 子窗口；失败返回 HRESULT，输出初始化为空 |
| `destroy(instance)` | 同模块内释放内存；能处理窗口已被系统销毁的情况 |
| `configure(instance, json)` | 更新 JSON 配置；成功返回 `S_OK`，无效配置返回失败 HRESULT |
| `layout(instance, width_px, height_px, dpi)` | 宿主已调整子窗口大小，通知可用像素区域和有效 DPI，重建绘制资源 |
| `theme(instance, dark)` | `dark=1` 为深色，`0` 为浅色；重新绘制内容 |

`WidgetHostApi` 提供：

- `log(context, level, utf8)`：写入管理器日志。
- `configuration_changed(context, json_utf8)`：插件内部操作改变设置时通知管理器保存；参数必须是 JSON 对象。
- `data_directory_utf8`：每实例独立目录，生命周期持续到 `destroy`；业务数据读写和迁移由插件自行负责。

传入字符串均为 UTF-8；除了数据目录字符串，其余只在当前调用期间有效，需要保留时自行复制。对象指针是不透明句柄。不得跨 DLL 边界传 STL 容器、C++ 异常、分配后交给另一个模块释放的内存；所有回调自行捕获异常。绘制应使用 `layout` 传入的 DPI，而不要假设 Explorer 的父窗口 DPI 就是所在显示器 DPI。

首版接口要求所有回调存在。设置持久化由管理器承担，便签文本、数据库和缓存等业务数据由插件自行管理。插件必须接受系统会回收整个宿主进程的事实，重要业务数据应及时提交。

宿主 UI 线程语言固定为 `zh-CN` 或 `en-US`。插件可在 UI 回调中用 `GetThreadUILanguage()` 获取语言；语言切换时宿主更新线程语言，并再次调用 `theme` 请求内容刷新。ABI v1 不变，旧插件仍可加载。系统区域可能是其他语言，日期请显式指定 `zh-CN` 或 `en-US`，时钟示例展示了处理方法。

## 内部通信

插件作者无需实现 IPC。管理器和宿主使用本机消息模式命名管道、重叠 I/O、当前用户 ACL、随机管道名以及对端进程验证。消息为 UTF-8 JSON，最大 1 MiB，包含 `protocol`、`op`、`id`、`seq`。该协议目前是内部实现，不作为第三方直接接入的稳定接口。

## 桌面与隔离边界

宿主负责顶部拖动区、尺寸限制、布局锁定、桌面挂接、主题通知及显示器变化。内容窗口仅占组件区域。不要修改父窗口或 Explorer 的窗口、消息循环和 DPI 模式。

添加、恢复和拖动结束时，宿主通过 Shell 的 `IFolderView` 只读查询桌面网格及图标位置，避让图标与其他组件。跨宿主的短期互斥锁保护位置分配，避免并发初始化选择同一个空位。网格位置以屏幕像素计算，再转换为显示器相对 DIP 保存。没有足够空位时报告错误并隐藏组件；不会自动移动桌面图标，也不会将插件注册为 Explorer 图标项。

实例保存 `gridColumns`、`gridRows` 作为尺寸来源，DIP 宽高仅保留为兼容与诊断缓存。宿主在 UI 线程接收调整尺寸请求，按当前格距换算并调用原有 `layout` 回调，ABI 不变；布局修订号防止旧布局消息覆盖较新的尺寸请求。低频查询缓存的 Shell 视图格距，变化后保持格数重排。

宿主容器采用 `WS_EX_LAYERED` 和不透明 Alpha，为使用 `WS_EX_NOREDIRECTIONBITMAP` 的 Windows 11 桌面提供独立合成表面。插件的子窗口可使用标准 GDI 绘制；Direct2D 可参考时钟，通过 `ID2D1DCRenderTarget` 向 `BeginPaint` 的 HDC 绘制。直接绑定 HWND 的 GPU 交换链需要另外验证与层叠父窗口的兼容性，不能仅凭创建成功判断内容已经显示。

当前支持 x64 原生 DLL；每个实例单独进程。进程隔离不是权限沙箱，请仅运行可信的原生插件。
