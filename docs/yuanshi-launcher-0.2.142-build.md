# 0.2.142 构建与离线回归

日期：2026-11-14（Asia/Singapore）。排障候选，不是完整可玩验收。

## 构建

- devkitA64 Release 完成编译、链接与 NRO 打包，无错误。
- `build/switch/yuanshi-launcher.nro` 与
  `dist/yuanshi-launcher-0.2.142.nro`：30,400,209 字节。
- SHA256：`69755480BEACA96A9F50EEDBEDBFA7BB6760517D5B5AB82057C4F1DF2ABC67DB`。
- 已部署到 yuzu apphome，部署后哈希一致。
- 仅有 vendored stb 初始化警告。
- 未提交、推送、发布 Release 或复制到真实 Switch。

## 离线回归

12 个目标通过：

`mihoyo_autotest_pad_policy`、`mihoyo_input_packet`、`mihoyo_rtc_session`、
`genshin_native_config`、`mihoyo_sdk_large_config`、
`mihoyo_launcher_input_policy`、`idle_gamepad_policy`、
`input_transport_policy`、`mihoyo_touch_forward`、`mihoyo_safe_event_log`、
`mihoyo_stream_input_policy`、`mihoyo_autotest_policy`。

## 在线验证

待填。
