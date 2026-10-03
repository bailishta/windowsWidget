# SDK 与组件接口

新版同时支持 schema 1 零代码组件和独立 DLL MOD。实际 C ABI 以 [WidgetSdk.h](../sdk/WidgetSdk.h) 为准，包格式见 [接入教程](PLUGIN_DEV.md)。

每个组件在独立进程运行；主程序只通过共享内存传递布局、主题、显示状态和通用命令。业务代码和内容界面都在 MOD DLL，新 MOD 不需要修改管理器。

## 原生接口

导出 HRESULT __cdecl WidgetGetApi(uint32_t requested_version, WidgetApi* api)。版本号仍为 WIDGET_ABI_VERSION == 1；WidgetApi2 / WidgetHostApi2 是通过结构大小协商的扩展布局，不是另一个 requested_version。DLL 为 x64，必须检查 size。

- create / destroy：创建和释放实例。
- layout / theme：内容区物理尺寸、DPI 与明暗主题变化。
- configure：可实现插件配置；创建时宿主传入独立 configuration.json 或清单 configuration。
- WIDGET_CAPABILITY_XAML：通过 host.ui_render 提供完整 UTF-8 XAML，宿主创建标准 WinUI 控件；命名 Button 的点击通过 ui_event(instance, x:Name) 返回。
- 无 XAML 标记：create 返回本进程内的子 HWND，parent 必须为传入宿主窗口；插件负责内容绘制。
- ui_event("refresh") / ui_event("settings")：控制中心的通用动作，插件自行响应。
- log / configuration_changed / data_directory_utf8：日志、配置保存和独立数据目录。

生命周期与 UI 回调运行在宿主 UI 线程。后台结果须回到所属 UI 线程再调用 ui_render，该函数拒绝跨线程操作。每次 XAML 最多 128 KB，返回 0 表示解析失败。不要在 DLL 内创建 WinUI Application 或资源提供者；使用宿主渲染或普通 Win32 子窗口。

固定宽度字段、UTF-8、不透明句柄；不跨 DLL 传 STL、异常或交叉释放。字符串在调用期间借用，data_directory_utf8 和 host 表在 destroy 前有效。插件必须结束线程并释放资源；卡住的组件由独立 Job 有界终止。

内容宽度 288 DIP（外框 320 DIP 减左右边距），清单高度 180–600 DIP。外框、标题拖动区、系统区域避让和统一动画由管理器负责，内容界面与业务由插件控制。支持自由组合标准 WinUI 控件，或使用原生 HWND 自行绘制；并不是四种固定内容模板。

缺少 DLL、路径越界、接口不兼容、启动超时和异常退出不会让整个管理器退出。这是进程故障隔离，不是恶意代码沙箱。只加载信任的 MOD。

完整独立示例见 [蓝牙电量插件](../plugins/bluetooth-battery/README.md)。旧桌面宿主保留于 src/host，早期文档在 docs/archive。
