# 0.2.114 本地构建与验证

日期：2026-11-07（Asia/Singapore）。基于 GitHub main
`0d9af76eba6d458879b3f8f8cdbb02c155cd9920`，包含 0.2.113 与本轮尚未提交的本地修改。
未创建 GitHub Release，未改写远程提交历史。

## 已完成

- `build-switch.ps1`：devkitPro / devkitA64 Release 编译、链接与 NRO 打包成功。
- `tests/yuanshi_nro_metadata_test.ps1`：NRO0、ASET、NACP、16 个语言槽、
  版本 0.2.114、作者 Steve1451-dot 与所选图标一致。
- 10 个相关主机测试目标通过：`audio_pipeline`、`audio_diagnostics`、
  `audio_jitter_policy`、`audio_latency_policy`、`audio_rtp_utils`、
  `audio_playback_timeline`、`media_pipeline_queue_model`、`mihoyo_ui_language`、
  `mihoyo_sdk_bridge`、`mihoyo_sdk_cloud_data_store`。
- 同一生产音频实现另有 11 个场景通过：时间线、换源、重新预缓冲、
  输出失败、背压、溢出重锚、长缺口重锚、20ms / 2.5ms / 120ms 包及自适应缓冲。
  合计 21 项检查通过；设备释放、故障和包时长测试使用离线模拟输入。
- `git diff --check` 通过。

## 最终 NRO

- 字节数：30330577
- SHA256：`EFCFE1EFEEF00E6317DB7DA869D389C628FCE63320D0CA16A06156DC90C5F5F7`

本包不含登录库、Cookie、二维码、账号令牌或测试会话数据。
更新只需替换 `switch/yuanshi-launcher/yuanshi-launcher.nro`，保留已有配置和登录数据。

## 验证边界

本轮读取了此前的 Switch 安全日志，但按用户要求，没有运行更新后的实机或模拟器游戏测试。
主机测试和 Switch 构建成功不等于已证明实际听感、所有网络条件下的恢复或长期稳定性。
0.2.113 的游戏原生键位跨会话恢复仍未完成实机验收。
