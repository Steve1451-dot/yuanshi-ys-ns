# 0.2.139 构建与在线对照

日期：2026-11-14（Asia/Singapore）。排障候选，不是完整可玩验收。

## 构建

- devkitA64 Release 实际完成 128 步编译/链接/NRO 打包。
- `yuanshi_nro_metadata_test.ps1` 通过：NRO0/ASET/NACP、版本、图标、作者及 16 个语言槽。
- `dist/yuanshi-launcher-0.2.139.nro`：30,400,209 字节。
- SHA256：`04A28216CFE4F4CE81D8E20E0FDAAD5D129E8797F2620E1AB56BD3FE75554C2F`。
- 已有第三方 stb 初始化警告，构建无错误。本轮未改 vendored 代码。
- 未提交、推送、发布 Release 或复制到真实 Switch。

## 离线回归

8 个目标通过：`mihoyo_autotest_pad_policy`、`mihoyo_input_packet`、`mihoyo_rtc_session`、
`genshin_native_config`、`mihoyo_sdk_large_config`、`mihoyo_launcher_input_policy`、
`idle_gamepad_policy`、`input_transport_policy`。

新增代码所在文件 `git diff --check` 通过。工作区原有的其他改动保持不动。

## 在线验证

0.2.138 Keybinds 与 Exact 短测均建立真实登录/媒体/SDK 会话，但截止时未见游戏进入信号。
0.2.138 Exact 长测（`reports/report-0138-exact-long-nov14/status.json`）末快照 596 秒，
解码 8,609 帧，仍未见游戏进入信号；约前 300 秒为排队。
0.2.139 Exact + Xbox 360 对照（`reports/report-0139-exact-xbox360-nov14/status.json`）
末快照 596 秒，PID 数值 654（`0x028e`），解码 5,180 帧，仍停在“点击进入”，
游戏进入信号为 false；约前 380 秒为排队。不能当成不同型号的解码性能对比。

本日共完成上述 4 轮真实云会话（2 轮短测、2 轮长测），均因测试时限结束，
不是报错退出，也不是可玩验证通过。已有登录正常恢复，无需用户重扫。
本地最终输入 trace 证明两种 PID 的 A 按下与松开均交给传输，但没有实际游戏响应证据。
此次比较不支持将默认 PID 改成 Xbox 360，故默认仍是 `0x045e/0x0b13`。

型号对照未解决标题页无响应，原生键位“进入游戏后正确采用、重启仍可用”仍未验收。
改动只修了诊断额度不足并增加受限对照能力，不能写成已修通游戏输入。
临时 Exact/型号标记已撤掉，4 份原始加密游戏配置已恢复并逐份验证哈希。
未读取或输出登录库内容；运行结束已关闭本轮所属模拟器，没有遗留排队/监控进程。

不能将离线报文通过、SCTP 写入或键位回写一致
当成云端输入有效、游戏已进入或重启映射已验收的证据。
