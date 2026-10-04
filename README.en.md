# WindowsWidget · Notification Companion

[简体中文](README.md) | [English](README.en.md)

WindowsWidget is a C++ / WinUI 3 companion for Windows 11. It displays independent widget cards beside Notification Center and Quick Settings, and hides them when the system panel closes. Your desktop stays clear while useful information is available alongside the Windows panels.

The control center lets you install, enable, configure, restart, arrange, and remove components. Each component runs in its own process, with its own window and data directory.

## Features

- **Optional widgets:** weather, to-do lists, CPU and memory charts, and three customizable shortcuts for apps, files, folders, or websites.
- **Custom components:** import JSON definitions for text, notes, checklists, and HTTPS links, or build native DLL plugins and standalone EXE components using the SDK.
- **Independent processes:** a component can be stopped or restarted separately. DLL plugins run in a separate host process.
- **Inline editing:** edit notes and to-do items directly in their cards. Notes save automatically, and completed tasks can be hidden without being deleted.
- **Flexible layout:** use automatic positioning beside the system panel, or drag card headers to save your own positions.
- **Native appearance:** Acrylic backgrounds, rounded cards, coordinated animations, and support for system light and dark themes.
- **Startup controls:** optional launch at sign-in, silent startup, and a setting to hide the tray icon. Automatic startup is disabled by default.
- **Bluetooth example:** an optional DLL component displays connected devices and reports battery levels when available.

The application supports Chinese and English. Launch it with `--en` to use the English interface.

## Install

1. Download the EXE installer from [GitHub Releases](https://github.com/bailishta/windowsWidget/releases).
2. Run the installer. It installs for the current user by default, includes the WinUI and Visual C++ runtime files, and offers an optional desktop shortcut.
3. Open the control center and install the components you want from the component library. A fresh installation starts with no components installed.

The default installation directory is `%LOCALAPPDATA%\Programs\WindowsWidget`. Upgrading and uninstalling preserve user data. The v0.1.0 installer is unsigned.

## Use

| Action | Shortcut or behavior |
| --- | --- |
| Open Notification Center | **Win+N** |
| Open Quick Settings | **Win+A** |
| Toggle manual widget preview | **Ctrl+Alt+W** |
| Exit the application and its components | **Ctrl+Alt+Q** |
| Hide the control center | Close its window; the application keeps running |
| Reopen the control center | Click the tray icon or run the application again |

Manual preview ends when a detected system panel takes over. Closing that panel hides the cards together. If the tray icon is hidden, running the application again still opens the control center.

Weather uses Open-Meteo by default and can be configured to use a custom HTTPS JSON API. To-do lists, notes, layout choices, and component settings are saved locally.

## Build from source

The current build targets **Windows 11 x64**. You need Visual Studio 2022 C++ Build Tools, Windows SDK **10.0.26100.0**, and the NuGet CLI. Restore the pinned dependencies into the repository's `packages` directory, then build from PowerShell:

```powershell
nuget restore .\demo\NotificationCompanion\packages.config -PackagesDirectory .\packages
.\demo\NotificationCompanion\build.ps1 -Test -Package
.\out\companion-demo\ControlCenter\WindowsWidget.exe --en
```

This builds the control center and optional component packages, runs the existing Chinese, English, and independent-process checks, and creates a distribution directory under `out\packages`.

To create an EXE installer, install Inno Setup 6.3 or later and run:

```powershell
.\build-installer.ps1 -Version 0.1.0
```

The installer is written to `out\installers\WindowsWidget-0.1.0-x64-Setup.exe`, with a SHA256 file and build metadata alongside it. See the [installer guide](installer/README.md) for compiler paths and validation commands.

## Components and local data

Import a component's `widget.json` through the control center, or place its package in the component directory and rescan. Imported components start disabled so you can review them before enabling them. The [component development guide](sdk/components/README.md) includes the schema, SDK interfaces, examples, and package limits.

The default data directory is `%LOCALAPPDATA%\WindowsWidget\CompanionDemo`:

- `manager.json` stores component order, enabled states, positions, theme, and tray preferences.
- `startup.json` stores the silent-start preference.
- `plugins` contains installed component packages.
- `plugin-data` contains each component's data and logs.
- `removed-plugins` contains recoverable archives of removed component packages.

Use `--data-dir` to select a separate data directory. Exit an older running build before switching versions; builds share a single-instance lock.

## Current limits

Windows does not provide a stable public timeline for Notification Center animations. The project checks panel identity before following it; individual notification banners do not trigger the cards. Real panel synchronization, multiple monitors, and fullscreen behavior still need testing across Windows versions. Component process isolation is not a security sandbox.

## More documentation

The detailed guides below are currently in Chinese.

| Guide | Contents |
| --- | --- |
| [Application guide](demo/NotificationCompanion/README.md) | Features, controls, and data |
| [Architecture](docs/ARCHITECTURE.md) | Control center and component design |
| [Component development](sdk/components/README.md) | Component schema, examples, and SDK |
| [Plugin development](docs/PLUGIN_DEV.md) | Native DLL and EXE integration |
| [Startup settings](docs/STARTUP.md) | Sign-in and background behavior |
| [Validation](docs/VALIDATION.md) | Recorded checks and practical limits |
| [Changelog](docs/CHANGES.md) | Project changes |
| [Dependencies and licenses](docs/THIRD_PARTY.md) | Dependency notices and attribution |
