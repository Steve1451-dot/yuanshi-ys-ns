# 0.2.116 本地构建与验证

日期：2026-11-08（Asia/Singapore）。基于 GitHub main
`0d9af76eba6d458879b3f8f8cdbb02c155cd9920` 和本工作区累计修改。
本轮没有提交、推送或创建 GitHub Release。

## 主机回归

10 个测试目标全部通过：`input_transport_policy`、`mihoyo_rtc_session`、
`mihoyo_control_channel_policy`、`mihoyo_launcher_input_policy`、`mihoyo_stream_input_policy`、
`mihoyo_ui_language`、`mihoyo_sdk_large_config`、`mihoyo_sdk_cloud_data_store`、
`controller_system_buttons`、`gyro_stick_assist`。

实际 Session 离线传输测试覆盖 8 个组合：ys／其他应用、云端手柄标志有／无、恢复配置有／无。
验证 RTC 尚未就绪时保留 GCC；目标 ys 强制 Xbox 路径在 RTC 就绪后使用 RTC；
其他路径保持 GCC 优先；切换时 ready 可一直为真而通道代次仍变化。
同时验证设备列表、输入来源、中性帧、按键真实编码、不双发、RTC 短暂掉线回退再恢复，
以及闲置 GCC 失败不重置正在工作的 RTC 手柄。

SDK 大配置的逐字节回传和独立明文／线上限回归继续通过。
fake transport error 是离线故障注入预期分支，不是线上登录结果。

## Switch 构建

- `build-switch.ps1`：devkitPro / devkitA64 Release 编译、链接、NRO 打包成功。
- 元数据检查通过：NRO0、ASET、NACP、16 语言槽、0.2.116 版本、所选图标、作者 Steve1451-dot。
- 第三方 stb_image_write 的既有缺省字段初始化警告不影响本次构建完成。
- `git diff --check` 通过。

最终 NRO：30338769 字节。

SHA256：`2B26E82003302E2562F9A0A74C3FA118ABD4FA756E7EC7C3354E7ABAC600E17A`

## 尚未验收

本轮读取的是用户上一版的实机安全日志；未主动运行本版实机或模拟器游戏。
离线测试不证明第二次进入的键鼠界面已消失，也不证明原生游戏键位已永久保存。
SDK 专用配置临时分析副本已清理；未读取登录凭据、改写原 SD 配置或恢复本地改键。
安装包不含账号、运行日志、登录库、SDK 用户配置、密钥、二维码或测试标记。
