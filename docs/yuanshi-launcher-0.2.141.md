# 0.2.141 标题页延迟声明手柄 + 触摸

日期：2026-11-14（Asia/Singapore）。排障 alpha，不代表已验收。

## 目的

0.2.140 第二轮在 mhyjx1 上只声明触摸（mask 8）、不宣布手柄列表，46 次触摸仍停在
标题页；SDK 登录在 183 秒完成、`GAME_SDK_KEEP_PLAYING_SENT` 在 220 秒，触摸并不算
打得太早。对照 0.2.133 成功时会话同时有手柄列表和 mask 12，因此下一步测试：
**标题页第一次触摸前补发手柄列表 + mask 12**，看 pod 是否必须先创建虚拟手柄才接受
触摸。

## 改动

- 新增测试标记 `cloudgenshin-autotest.touch-first-late`（内容
  `CGNX_TOUCH_FIRST_LATE_V1`）。存在时沿用触摸优先：进入前只声明 mask 8、不提前
  宣布手柄列表。
- 标题页第一次触摸按下前，若尚未声明，则依次发送：
  1. `GAMEPAD_LIST`（Xbox One S `0x045e/0x0b13`）；
  2. `INPUT_SOURCE_REDECLARED_AT_TITLE`（触摸终端为 mask 12）；
  3. 原有的触摸 Down。
- 发送失败会在下一次触摸尝试前重试；成功只发一次。进入游戏后仍走已有的
  `GAMEPAD_REDECLARE_AFTER_GAME_ENTRY` 路径。
- `app/src/app_version.hpp`：`0.2.141`。

普通用户路径、bt、zmd、键位回写、媒体策略均不变。

## 产物

`build/switch/yuanshi-launcher.nro` / `dist/yuanshi-launcher-0.2.141.nro`：
30,400,209 字节，SHA-256
`48690C75CF6645AACF84E92EDC195BD601C0BB889685908F21C3CED6B7B47E9F`。
已部署到 yuzu apphome，部署后哈希一致；测试标记为
`cloudgenshin-autotest.touch-first-late`（内容 `CGNX_TOUCH_FIRST_LATE_V1`）。

## 判定标准

进入前应看到：

- `INPUT_SOURCE_DECLARED code=8`（通道打开时只声明触摸）；
- 无 `GAMEPAD_LIST_ANNOUNCED`；
- 标题页首次触摸前出现 `GAMEPAD_LIST_ANNOUNCED_AT_TITLE` 与
  `INPUT_SOURCE_REDECLARED_AT_TITLE code=12`，随后才是 `AUTO_ENTRY_TOUCH_PRESS` /
  `AUTO_ENTRY_TAP_SENT`。

进入后应看到 `GAMEPAD_REDECLARE_AFTER_GAME_ENTRY`，并重新出现手柄列表与
`INPUT_SOURCE_DECLARED code=12`。只有 `GameEntrySignalObserved=true` 才算生效；
ACK / send 事件本身不是送达证明。

## 若仍失败

按顺序测试两个变体，并记录 pod：

1. 只补手柄列表、保持 mask 8；
2. 只补 mask 12、不补列表。

若两个变体都失败，而 0.2.133 在 jx9 上同样输入成功，则优先归因于 mhyjx1
pod 差异，而不是声明顺序；需要重抽非 mhyjx1 节点再验证。

## 第一轮 report-0141-soak-late-declare：被物理手柄打断，无效

2026-11-14 12:00 启动，队列一度到 10 位并进入 `PreparingQueue`，但 352 秒出现
`LANGUAGE_SAVED`，403 秒与 448 秒连续 `CANCEL_REQUESTED` / `LAUNCH_CANCELLED`，
随后重新取号。660 秒主动停止时仍在排队（31 位），没有出图。

原因：机器上连着的 Xbox 手柄（`VID_045E/PID_0B12`）输入仍会进入 yuzu；隐藏窗口
只挡住鼠标点击，不挡模拟器收到的本地手柄按键。用户已拔掉手柄。之后清掉 apphome
残留的 `cloudgenshin-autotest.stop`，用 `-RunTimeoutSeconds 2400` 重开
`report-0141-soak-late-declare-2`。

## soak 结果

待填。
