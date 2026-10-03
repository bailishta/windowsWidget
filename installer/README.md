# EXE 安装包

使用 Inno Setup 6.3 或更新版本（本机使用官方 Inno Setup 7.1.0 x64），为当前 Notification Companion 控制中心生成 Windows 11 x64 安装包。

从项目根目录运行：

```powershell
.\build-installer.ps1 -Version 0.1.0
```

默认重新编译主程序、四个独立组件和蓝牙 DLL，并运行现有中英文及独立进程测试。输出为 `out/installers/WindowsWidget-0.1.0-x64-Setup.exe`，旁边提供 SHA256 与构建来源记录。安装包自包含 Windows App SDK 和 VC++ 运行库。编译器可从 [Inno Setup 官方页面](https://jrsoftware.org/isdl.php) 获取；脚本优先查找项目 `.tools/inno-setup/ISCC.exe`，也支持 `-CompilerPath`。

已有完整发布目录时，可跳过编译：

```powershell
.\build-installer.ps1 -SkipBuild -SourceDirectory .\out\companion-demo\Installer -Version 0.1.0
```

- 默认按当前用户安装到 `%LOCALAPPDATA%\Programs\WindowsWidget`，不要求管理员权限，可选择其他目录。
- 创建开始菜单快捷方式，桌面快捷方式默认不勾选；静默安装不会启动程序。
- 安装及卸载通过现有退出处理器正常关闭控制中心，它负责保存数据、关闭组件进程；10 秒后仍未退出则停止操作，不强杀进程。
- 升级使用固定 AppId、沿用原安装目录和选择，更新安装器管理的文件。用户安装的组件仍由主程序管理，安装器不覆盖其数据目录。
- 开机启动仍由主程序控制。卸载只清除指向本安装目录的启动项，保留其他目录版本的启动项。
- 卸载保留 `%LOCALAPPDATA%\WindowsWidget\CompanionDemo` 中的配置、已导入组件、待办及其他用户数据。
- 没有代码签名证书时输出为未签名安装包。正式发布可配置 Inno Setup 的签名工具。
- 版本号由 `-Version` 指定，每个数字部分为 0–65535；发布新版本时应递增。默认 `0.1.0` 是当前初始安装版的版本号。

安装器不会删除用户自行放进安装目录的额外文件，也不会对整个安装目录做递归清空。调试符号和测试日志不会打入安装包。

## 安装器验证

```powershell
.\installer\test-installer.ps1
```

测试要求没有已安装或运行中的 WindowsWidget。它会暂时登记当前用户卸载项，在项目 `out/installer-tests` 下的中文带空格路径安装，创建独立测试快捷方式，用隔离数据启动待办组件，然后验证运行中升级、卸载、快捷方式清理及用户数据保留。成功后卸载测试程序，保留日志与隔离数据；不安装到日常使用目录，不改动现有自启动设置。报告保存在对应测试目录的 `validation.json`。
