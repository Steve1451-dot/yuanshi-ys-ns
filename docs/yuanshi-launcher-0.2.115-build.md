# 0.2.115 本地构建与验证

日期：2026-11-08（Asia/Singapore）。基于 GitHub main
`0d9af76eba6d458879b3f8f8cdbb02c155cd9920`，包含 0.2.113、0.2.114 及本轮本地修改。
本轮未提交、推送或创建 GitHub Release。

## 回归证据

- 新增 `mihoyo_sdk_large_config` 用例，在修复前确实因 87834 字节配置穿不过控制编码而失败。
- 修复后 13 个主机测试目标全部通过：`mihoyo_sdk_large_config`、`mihoyo_rtc_session`、
  `mihoyo_game_control`、`mihoyo_sdk_bridge`、`mihoyo_sdk_cloud_data_store`、
  `mihoyo_ui_language`、`mihoyo_signaling_frame`、`mihoyo_sdk_json`、`mihoyo_safe_event_log`、
  `controller_system_buttons`、`gyro_settings`、`gyro_stick_assist`、`endfield_xinput_state`。
- 大配置覆盖 87834／200000 字节、转义字符与 NUL 的逐字节恢复、SDK 接收线上限、
  匹配／错误／重复 ACK、发送失败，以及 SDK 专属 512 KiB／其他命令 64 KiB 边界。
- 实际 `Session`、signaling client、WebSocket 编码的离线用例确认 87834 字节配置能
  完整通过；没有空 proxy 冒充 SDK 回复。传输端为合成的离线 fake transport。
- 故障注入中的 fake transport error 为预期测试分支，不是线上连接结果。
- `git diff --check` 通过。

## Switch 构建

- `build-switch.ps1`：devkitPro / devkitA64 Release 完整编译、链接和 NRO 打包成功。
- NRO 元数据检查通过：NRO0、ASET、NACP、16 语言槽、版本 0.2.115、
  Steve1451-dot 作者及所选图标一致。
- 第三方 stb_image_write 的既有缺省字段初始化警告不影响构建完成。

最终 NRO：30330577 字节。

SHA256：`EDA3FBC92154B4831CD335D423149DEB537F6E94EE79D7AA26CFA66A59FA53C6`

## 边界

按用户要求未启动本版实机或模拟器游戏测试。
已修复可复现的配置传输阻断，但尚未验收游戏是否采用原生手柄绑定。
包内不含登录库、账号凭据、SDK 用户配置、密钥、二维码或运行日志。
