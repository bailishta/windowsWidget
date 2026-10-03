# 蓝牙设备电量 MOD

独立 C++ DLL，使用 WidgetSdk.h；蓝牙业务和四行 XAML 全部在此源码，不属于主程序业务。自己修改或编译它不需要重编主程序。

```powershell
./plugins/bluetooth-battery/build.ps1
```

需要 Visual Studio 2022 C++ Build Tools 和 Windows SDK 10.0.26100.0。包输出在 out/mods/bluetooth-battery，通过控制中心“添加组件文件”选择其中 widget.json 后启用。完整发布版提供“添加蓝牙设备电量”。

后台每 30 秒查找已连接的 classic / LE 设备，按容器 ID 或地址去重；现有设备保持槽位，断开后清空。固定四行，超过四个提示剩余数量。组件只负责显示，不提供设置或编辑入口，清单通过 has_settings=false 隐藏控制中心的设置按钮；卡片的手动刷新仅重新读取设备状态。

优先读取 System.Devices.BatteryLife，补充读取 Windows 驱动暴露的可选 PnP battery 属性；BLE 使用标准 Battery Service / Battery Level。只有真实返回 0–100 时显示百分比，未知显示“设备未提供电量”，没有设备时保留四个空位。不配对、不安装驱动；权限不足或查询超时显示状态，不阻塞宿主 UI。

XAML 使用 ThemeResource 跟随宿主明暗主题，外框、标题、尺寸和动画由宿主管理。内容可以自行换成其他界面，不受便签或文字模板限制。

last-reading.json 保存最近真实检测状态，可排查硬件支持，不作为伪造电量缓存。configuration.refresh_seconds 支持 10–600 秒，默认 30。

参考官方 API：

- [已连接 BLE 设备选择器](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.bluetoothledevice.getdeviceselectorfromconnectionstatus)
- [System.Devices.BatteryLife](https://learn.microsoft.com/en-us/windows/win32/properties/props-system-devices-batterylife)
- [GattServiceUuids.Battery](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.genericattributeprofile.gattserviceuuids.battery)

驱动 PnP 属性仅为可选兼容路径，不保证每个 Windows 驱动或耳机型号支持。实际读数由设备是否提供决定。
