# 日志与故障定位

在管理器底部点击“打开日志 / Open logs”可打开日志目录。默认路径是 `%LOCALAPPDATA%\WindowsWidget\logs`；使用 `--data-dir` 时位于指定数据目录的 `logs` 子目录。

| 文件 | 记录内容 |
| --- | --- |
| `ui.log` | 程序启动与退出、构建时间和运行目录、主题、托盘操作、界面操作错误、启动失败及 WinUI 未处理异常 |
| `manager.log` | 配置恢复与保存、插件扫描、实例增删与启停、尺寸与布局、宿主 PID、握手和就绪耗时、加载失败、宿主异常退出、插件日志 |

每个文件达到 5 MiB 时轮转，保留当前文件及 `.1`、`.2`、`.3` 三份备份，`.1` 最新。两个日志流正常总量上限约 40 MiB。默认记录 INFO 及以上级别，无需配置即可使用。

## 开启详细日志

先从托盘退出旧进程，再从程序目录运行：

```powershell
.\WidgetManager.exe --log-level debug
```

支持 `debug`、`info`、`warning`、`error`。级别只影响本次进程，DEBUG 额外记录界面操作源码行号和 IPC 操作名称、请求序号。若已有主程序驻留，新进程只唤回原进程，不会改变其日志级别。

日志为 UTF-8 单行文本，例如：

```text
2026-09-20T05:07:38.809Z [ERROR] pid=162660 tid=244496 uptime_ms=437060656 [manager-ui] UI action failed source_line=414 diagnostic validation error
```

时间使用 UTC（末尾 `Z`）；中国标准时间需加 8 小时。`pid`、`tid` 是产生日志的管理器进程和线程；`uptime_ms` 是系统启动后的毫秒数，可用于同次系统运行中的时间差比较。实例事件包含完整 `instance=<id>`，插件转发日志另含宿主 `host_pid` 和插件上报的 `host_tid`。错误尽可能包含 HRESULT 或宿主退出码。

## 查看与反馈

```powershell
$logDirectory = Join-Path $env:LOCALAPPDATA 'WindowsWidget\logs'
Get-Content (Join-Path $logDirectory 'manager.log') -Encoding utf8 -Tail 80 -Wait
# 另一个终端：筛选当前及轮转文件中的错误
Get-ChildItem -LiteralPath $logDirectory -File |
    Select-String -Encoding utf8 -Pattern '\[ERROR\]|\[FATAL\]'
# 按实例 ID 关联扫描、启动、布局、退出等事件
Get-ChildItem -LiteralPath $logDirectory -File |
    Select-String -Encoding utf8 -SimpleMatch 'instance=<完整实例 ID>'
```

轮转后可重新运行 `Get-Content` 查看当前文件。反馈问题时提供操作时间、复现步骤，以及 `ui.log`、`manager.log` 和相邻备份。配置变更只记录事件或字节数，不主动记录配置 JSON 正文；插件自行提交的日志和异常消息保留其诊断内容，作者应避免在其中输出业务数据。

## 实现与边界

- 后台线程执行文件写入和轮转，调用线程只格式化并入队。队列最多 4096 条；溢出时高等级消息可替换低等级消息，记录丢弃数量，管理器底部提示。所有等级都属于有界、尽力记录。
- 单条消息最长 8192 字节，按 UTF-8 边界截断，换行转义以保持单行。不同线程安全写入，相同路径的跨进程写入和轮转通过命名互斥锁串行化。
- 写入失败不会作为日志异常传播到业务调用方；计数并输出调试消息，管理器底部提示检查目录权限与磁盘空间。
- 正常退出排空队列；ERROR/FATAL 在后台写入后调用 [FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)。突然断电、强制终止或进程崩溃仍可能丢失队列中的记录。
- [WinUI UnhandledException](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.ui.xaml.application.unhandledexception?view=windows-app-sdk-1.8) 记录后维持默认异常处理，不吞掉致命异常。它不能覆盖全部原生崩溃；本版没有崩溃转储或调用栈采集。

无需更改插件 ABI。插件日志级别及线程契约见 [SDK 文档](SDK.md)。
