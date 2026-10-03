# 日志

启动设置新增事件：`manager_hidden` 记录主窗口隐藏；`autostart_changed` / `silent_start_changed` 的 `enabled` 记录成功保存的开关；`startup_setting_failed` 包含错误信息和错误码；`startup_preferences_invalid` 表示原设置文件保留并使用安全默认值。`startup` 的 `autostart_launch` 与 `silent_start` 可区分登录启动及静默偏好。隐藏窗口测试另有 `startup_settings_test_pass` / `manager_hide_test_pass`，只代表隔离自动检查完成。

## 独立 demo

%LOCALAPPDATA%\WindowsWidget\CompanionDemo\companion.jsonl，UTF-8 JSONL。超过约 5 MiB 轮换为 companion.previous.jsonl。UI 格式化后入有界队列，后台写入/刷新/轮换；队列最多 2048 条，溢出记录 log_queue_overflow 和丢弃数量。正常进程退出等待日志队列写完，异常终止可能丢失末尾记录。

| 事件 | 意义 |
| --- | --- |
| startup / startup_failed | 启动、WinUI 错误 |
| entry_failed | 入口初始化失败，记录 HRESULT 与消息 |
| candidate_state / no_candidate | 候选与状态变化 |
| observer_ready / observer_pending | 观察快照是否可用，根窗口数与 UIA 状态 |
| shell_inventory | Shell 原生范围、内容范围、识别依据与 UIA 结果 |
| preview_enabled / preview_disabled / filter_changed | 预览和筛选状态切换 |
| panel_shown / panel_repositioned / panel_hidden | 首次显示、已显示位置变更、隐藏 |
| backdrop_configured / backdrop_unavailable | 系统 Acrylic / 不透明回退及连接错误 |
| motion_enter_started / motion_enter_completed | 整个面板开始入场、完成 |
| motion_exit_started / motion_exit_completed | 整个面板开始退场、完成 |
| shell_exit_early | 根据已观察的 Shell 激活链提前退场 |
| log_queue_overflow | 日志队列满时丢弃数量 |
| mouse_activate_blocked / unexpected_panel_activation | 鼠标策略、异常激活 |
| pointer_press / click_probe | 点击关联与多次采样 |
| theme_changed / system_theme_changed / system_theme_enabled | 手动明暗切换、系统主题变化、恢复跟随 |
| preview_hotkey_unavailable / quit_hotkey_unavailable | 注册冲突 |
| tray_unavailable / set_position_failed / poll_failed | 基础设施错误 |
| self_test_started / native_motion_fixture_pass / self_test_pass | 隐藏自测、原生动画循环、断言数 |
| shutdown | 关闭 |

公共字段 time（UTC）、tick_ms、event、foreground。候选含 HWND、类型提示、进程/类、cloak 与范围；不记录其他应用标题或通知内容。

修正版 startup 包含 build=panel-follow-6、follow_policy=confirmed_panels_only、alignment=bottom、enter_ms / exit_ms、animations_enabled、原生/UI 目标周期、panel_hwnd 与 control_hwnd，便于确认运行的版本。候选的 evidence、geometry_source、uia_status、uia_nodes、identity_bits 区分识别不足、提供者失败和宿主矩形过大；native_* 与候选内容范围分别记录，panel_shown/panel_repositioned 的 left/top/right/bottom 是最终布局位置。只记录单个窗口的实际变化，忽略单独的节点数波动。UIA 仅查询，不触发操作，不把通知正文写入日志。

shell_inventory 的 follow_eligible 表示窗口能否用于自动跟随；未确认身份时 follow_exclusion=panel_identity_unconfirmed。消息横幅可出现在观察清单，但不出现在自动候选中；这条排除理由表示缺乏面板证据，不声称已识别所有未知窗口的真实用途。

motion_enter_started 的 axis / duration_ms / reversed 表示方向、当前剩余路径的时长和是否反向。完成事件表示程序已提交最终窗口位置，不证明画面已在屏幕显示；motion_frame_requests 只统计工作线程请求，不是实际刷新帧率。backdrop_configured 表示选择的材质，实际透明效果仍可能因系统策略回退。

panel_shown 的 observed_to_show_ms（可用时）表示原生采样首次观察到打开至请求显示的时间；它不包括采样前的真实 Shell 打开时刻，也不表示首帧已绘制。native_sample_ms 是该次原生采样耗时。隐藏自测记录 native_samples、uia_passes、native_sample_age_ms 及人为 UIA 等待；不能当作真实动画帧率。

action_id 关联点击。delay_ms 是目标延迟，elapsed_ms 是实际延迟。foreground_before 是收到控件按下时的前台；foreground_same 是采样比较。shell_observed 与 shell_open 分开表示身份存在/状态打开。

## 解释

有 panel_shown 却无画面：检查位置、DPI、层级和实际截图，日志不能证明像素绘制。shell_open 变 false 而 foreground_same 为 true，说明这次采样未观察到前台变化，但不能单独证明关闭原因。

unexpected_panel_activation 要检查控件真实行为。follow_eligible=false 表示类型未确认或不支持，因此不能自动跟随；可使用手动预览。托盘和日志目录属于主动管理，不当作卡片测试。

历史主程序异步 manager.log / ui.log、备份与级别说明见 [旧日志文档](archive/2026-10-02-before-companion/LOGGING.md)，本轮未复测，不能混用格式。

正式框架统一关联实例/请求/世代，记录发现/启动/握手/就绪、配置恢复来源、取消原因。日志失败不得清空配置，不记录敏感业务内容。
