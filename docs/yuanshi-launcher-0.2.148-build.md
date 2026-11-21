# 0.2.148 构建与验证

2026-11-16（Asia/Singapore）。本地调度与日志优化候选，尚无本版实机卡顿对照结果。

- devkitA64 / libnx 实际 Switch NRO 完整构建通过；最后改动后再次执行依赖增量构建并重新打包 NRO。
- NRO0、ASET、选定图标、16 语言 NACP 名称/作者/0.2.148 版本检查通过。
- 24 项相关 C++ 主机测试通过，音频另有 12 个子场景通过。
- 组合键接线保护、移动端入口基线、安全监控、MTP 非覆盖部署四项 PowerShell 检查通过。
- Bash 测试入口语法检查通过；Linux CI 增加后台日志、安全日志并发与音频诊断场景。
  此项是本地语法检查，不等于本次 GitHub Actions 已执行。

主机回归集合：`worker_scheduling`、`bounded_diagnostic_writer`、`mihoyo_safe_event_log`、
`mihoyo_safe_log_archive`、`mihoyo_safe_log_writer`、`endfield_peer_poll_policy`、`audio_pipeline`、
`media_pipeline_queue_model`、`mihoyo_rtc_session`、`mihoyo_ui_language`、`gyro_stick_assist`、
`gyro_settings`、`controller_system_buttons`、`mihoyo_input_packet`、`mihoyo_stream_input_policy`、
`mihoyo_launcher_input_policy`、`mihoyo_stream_overlay_policy`、`compact_stream_menu`、
`adaptive_stream`、`keyframe_request_policy`、`frame_recovery_timings`、`decode_recovery_state`、
`audio_jitter_policy`、`stage_timings`。

覆盖阻塞日志写入期间生产者继续提交、记录/字节/会话容量上限、异常写入、停止刷新、
真实 AudioPipeline 诊断接线、并发日志快照顺序、680 毫秒单次收包模拟、
优先级许可选择、媒体队列和关键帧重同步、即时体感及既有 Xbox/移动端入口。
未进行在线登录排队、yuzu 或 Switch 新版游玩验证，不能据此给出实机帧率提升百分比。

| 项目 | 值 |
| --- | --- |
| NRO | `yuanshi-launcher-0.2.148.nro` |
| 字节数 | 30,428,881 |
| SHA256 | `52637767A654DFBAEA75DAD1122F44F229CC196F2E87D988E38380CF305E88B2` |

安装包仅包含公开静态发布文件与本次 NRO，不含登录库、密钥、配置快照、日志、APK 或测试运行数据。
没有修改 0.2.147 的 NRO、标签或 GitHub Release；本版先作为本地候选交付。
