# 自定义组件与独立 MOD 接入

给用户与 AI Agent 的完整开发规范见 [组件目录开发指南](../sdk/components/README.md)，包含类型选择、当前限制、后台任务、数据保存、交付验收和可复制的 Agent 提示词。构建会随程序分发指南、SDK头文件与Schema；控制中心首次发现缺少这些文件时会将其复制到用户组件目录，不覆盖已有文件。

主程序支持零代码组件、真正可编程的 DLL MOD 和独立 EXE 组件包。天气、待办、常用入口、系统状态也已成为普通可选 EXE 包，均可安装、启用、卸载；新用户从组件库选择，旧用户原有安装选择和数据会迁移。每个组件有独立进程与数据目录，管理器统一控制启停、主题、动画、顺序和自由位置。蓝牙功能不包含在主程序内。

## 编写 MOD

一个目录包含 widget.json、x64 DLL 和可选依赖/资源。例如：

```json
{
  "schema": 1,
  "id": "user.my-mod",
  "name": "我的组件",
  "kind": "native",
  "abi": 1,
  "entry": "MyMod.dll",
  "height": 300,
  "configuration": {}
}
```

DLL 导出 WidgetGetApi，使用 [WidgetSdk.h](../sdk/WidgetSdk.h)。可任意编写数据源、算法、后台任务和内容界面。声明 WIDGET_CAPABILITY_XAML 后，通过 host.ui_render 提供完整 UTF-8 XAML，标准 WinUI 控件可以自由组合；命名 Button 的点击通过 ui_event(instance, x:Name) 返回插件。也可返回属于组件进程的普通 Win32 子 HWND，自行绘制内容。DLL 内不创建 WinUI Application；WinUI 控件由宿主管理。详细生命周期见 [SDK](SDK.md)。

点击“添加组件文件”，选择包内 widget.json。管理器复制整个目录，包括 DLL、资源和私有依赖；新导入默认停用，启用后在独立进程加载。DLL 私有依赖从 DLL 所在目录查找。以后接入新 MOD 无需修改或编译主程序。

包最多 50 MB、256 个文件，不允许链接和目录联接。entry 必须是包内存在的相对 DLL 路径。清单最多 64 KB，schema 为 1；id 全局唯一、1–64 字符，仅小写字母、数字、点、横线，不能以点开头/结尾或使用 Windows 保留文件名。name 1–80 字符，description 最多 300 字符。宽度由管理器统一为 320 DIP，height 180–600 DIP。

可选 `"icon": "icon.png"` 指定包内 PNG，用于控制中心列表和卡片标题，文件最多 4 MiB，路径不能越界。零代码组件导入时也会复制声明的图标；未指定时使用宿主按类型提供的默认图标。主程序、官方组件、用户组件类型和蓝牙 MOD 已提供统一风格的模型生成图标，原图及提示词在 assets/icons。

这是进程故障隔离，不是恶意代码沙箱，只启用自己编写或信任的 MOD。

## 安装与更新

已安装包在 %LOCALAPPDATA%\WindowsWidget\CompanionDemo\plugins/<id>/。重复 id 的导入不覆盖现有组件。停用后替换包内 DLL/资源，点击“重新扫描”再启用即可更新，也可以将完整包直接放到组件目录后重新扫描。

独立数据在 plugin-data/<id>/。卸载停止独立进程并归档完整安装包，保留用户数据；配置可通过 host.configuration_changed 保存，重启时恢复。一个组件的崩溃、停用或重启不影响其他组件；主程序退出时清理全部组件进程。

## 蓝牙电量 MOD 示例

源码在 plugins/bluetooth-battery。单独编译：

```powershell
./plugins/bluetooth-battery/build.ps1
```

得到 out/mods/bluetooth-battery/widget.json 和 BluetoothBattery.dll，导入这个 widget.json 即可。发布版 examples/bluetooth-battery 也带有编译好的包；控制中心“添加蓝牙设备电量”调用相同通用导入接口。

蓝牙扫描、真实电量读取、四行 XAML 和刷新按钮全部由独立 DLL 提供，宿主没有蓝牙 API 或专用蓝牙类型分支。固定保留四个设备位，只列出当前已连接设备；断开后清空，未知电量显示“设备未提供电量”，不制造百分比。默认每 30 秒刷新，支持手动刷新。更多说明见 [蓝牙 MOD](../plugins/bluetooth-battery/README.md)。

## 零代码组件

组件库提供便签、文字和蓝牙电量示例；“我的清单”和“开发入口”重复示例已移除，也可导入自己设计的 text / note / checklist / links。示例在 sdk/components/examples，Schema 在 sdk/components/widget.schema.json。待办和便签直接在组件卡片内编辑，无需独立编辑窗口。

text / note 的 text 最多 4000 字符；checklist 的 items 最多 30 条字符串，每条 1–200 字符；links 的 items 包含 title 和完整 HTTPS url。便签和清单初始化后保留编辑状态。零代码类型不接受 entry、dll、script；业务代码用 native 接口。

组件默认随系统通知中心或快速设置显示和隐藏，自动排列靠右下。关闭自动排列后可拖动整个组件标题，自由位置持久保存，允许组件互相重叠，避开任务栏和已识别系统面板。三点菜单已移除，管理功能集中在主控制中心。

独立 EXE 组件使用 kind=process、abi=1、entry=包内 .exe，通过 PluginWire 通道接入统一动画、布局与管理。包上限 256 MiB、2048 文件；四个标准组件的参考源码和独立项目见 plugins/companion。完整进程协议与装卸规则见 sdk/components/README.md 第 12 节。卸载会停止独立进程并归档完整包，保留 plugin-data；同 id 可以重新安装。
