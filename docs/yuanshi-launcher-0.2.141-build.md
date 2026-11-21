# 0.2.141 构建与在线验证

日期：2026-11-14（Asia/Singapore）。排障候选，不是完整可玩验收。

## 构建

- devkitA64 Release 实际完成 128 步编译/链接/NRO 打包。
- `build/switch/yuanshi-launcher.nro` 与 `dist/yuanshi-launcher-0.2.141.nro`：
  30,400,209 字节。
- SHA256：`48690C75CF6645AACF84E92EDC195BD601C0BB889685908F21C3CED6B7B47E9F`。
- 已部署到 yuzu apphome，部署后哈希一致。
- 仅有 vendored stb 初始化警告，构建无错误。
- 未提交、推送、发布 Release 或复制到真实 Switch。

## 离线回归

8 个目标通过：`mihoyo_autotest_pad_policy`、`mihoyo_input_packet`、`mihoyo_rtc_session`、
`genshin_native_config`、`mihoyo_sdk_large_config`、`mihoyo_launcher_input_policy`、
`idle_gamepad_policy`、`input_transport_policy`。

## 在线验证

待填。
