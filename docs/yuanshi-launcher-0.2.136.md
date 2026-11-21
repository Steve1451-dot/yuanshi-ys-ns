# 0.2.136 输入源声明周期重发实验

日期：2026-11-12

## 目的

0.2.135 的延后声明实验（pod jx9）证明：完全不发输入源声明时，标题页触摸同样
无效，pod 还在 200 秒主动关闭会话。结合 0.2.133 jx9 成功、0.2.134 慢 pod 失败
的时间线，当前假设是：

- 首次 `INPUT_SOURCE_DECLARED`（type-31，mask 12）在输入通道刚打开时发送；
- 快 pod 的输入适配器此时已就绪，声明被接受，触摸可用；
- 慢 pod 的标题页/输入适配器要到 90–110 秒才就绪，57 秒的声明被丢弃；
- 代码只按“SCTP 写成功”置位 `input_source_declared_`，没有重发，也没有收到
  pod 的 20037 `SyncGameControlMode` 回执。

本轮保留手柄列表和首次声明（恢复 0.2.134 行为），在 `launcher_entered_` 之前
每 4 秒重发一次同样的声明，验证慢 pod 是否能在适配器就绪后接受声明、恢复触摸。

## 改动

- `app/src/cloudgenshin_stream_view.cpp`：
  - 撤回 0.2.135 的 `defer_pad_until_entry` 实验。
  - 新增 `redeclare_input_until_entry`，仅在 `automated_seconds_>0` 且
    `cloudgenshin-autotest.redeclare` 存在时启用。
  - 首次声明成功后记录 `next_input_source_attempt=now+4s`；进入游戏前到点重发，
    成功记 `INPUT_SOURCE_REDECLARED`，失败 500 ms 后重试。
  - 输入通道重建或游戏进入触发的 redeclare 会把重发计时器重置为立即。
- `app/src/app_version.hpp`：`0.2.136`。

普通用户路径不变：没有标记时只发送一次声明，仍由输入通道重建和游戏进入
重新声明。

## 产物

`build/switch/yuanshi-launcher.nro`：30,387,921 字节，SHA-256
`B700E597C6BA905B05431063AAC96CF5C18E158F4C37D30C00A6CEAE2B890889`。
已部署到 yuzu apphome，部署后哈希一致；已写入
`cloudgenshin-autotest.redeclare`（内容 `CGNX_REDECLARE_V1`），并移除
`cloudgenshin-autotest.defer-pad`。

## soak 结果

待填。
