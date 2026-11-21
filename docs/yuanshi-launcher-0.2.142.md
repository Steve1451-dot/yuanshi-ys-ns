# 0.2.142 标题页触摸坐标与 Down→Move→Up

日期：2026-11-14（Asia/Singapore）。排障 alpha，不代表已验收。

## 背景

0.2.141 第二轮在 mhyjx1 上完成了标题页延迟声明：

```
INPUT_SOURCE_DECLARED=8                 (495s)
INPUT_SOURCE_REDECLARED_AT_TITLE=12     (545s)
AUTO_ENTRY_TAP_SENT=60
GameEntrySignalObserved=false
```

`sdk-bridge.log` 显示 60 次触摸持续发出，但标题页始终没有进入。复核
`frame_dump_11.bmp` 后确认：

- 新标题页页脚为 `CNRELWin7.2.0_R49411677_S4929486_D49419668`，客户端已在
  11-13 夜间更新；0.2.133/0.2.138 成功时页脚为 `R49086080`。
- “点击进入”文字中心约在 y≈0.930，旧坐标 0.947 是半透明条的底缘，可能落在
  更新后更紧的 hitbox 之外。
- 官方 `TouchController` 的触摸序列是 Down→Move→Up；此前自动进入只发
  Down→Up。0.2.141 在延迟声明后立刻按下，也没有给 pod 创建虚拟手柄留时间。

因此 0.2.142 只改自动进入触摸本身，不改认证、媒体、键位回写、手柄 wire
format 或普通用户路径。

## 改动

`app/src/cloudgenshin_stream_view.cpp` / `.hpp`、`cloudgenshin_stream_input.cpp`：

- 新增 `send_launcher_touch(session, action, x, y)`，自动进入在触摸终端上直接
  指定 Down/Move/Up；`send_launcher_pointer` 的既有语义不变。
- 标题页 y 坐标循环：
  `0.930 ×3 → 0.947 ×3 → 0.915 → 0.960`。每完成一次点击后按下一次点击序号
  取下一个坐标，避免一个错误坐标耗尽全部 60 次预算。
- 每次按下后 50 ms 发一次 `kTouchActionMove`，120 ms 后发 Up；Up 使用与 Down
  相同的 y。若调度跳过了 50 ms 窗口，释放前仍会补发 Move，保证
  Down→Move→Up。
- 触摸优先延迟声明成功后，等待 700 ms 再发第一次触摸，给 pod 创建虚拟手柄和
  处理 mask 12 的时间；声明未成功时每 300 ms 重试，不提前点击。
- 输入通道重建时清除 `touch_first_late_declared`、延迟截止时间和 Move 状态，
  重连后重新执行标题页声明。
- 新增 `.touch-initial-12` 测试标记：存在时取消触摸优先，回到 0.2.133/0.2.138
  的“初始 mask 12 + 手柄列表 + 纯触摸”对照，并关闭测试用手柄 A 兜底。

证据事件：

- `AUTO_ENTRY_TOUCH_Y`（code 为 y×1000，例如 930）
- `AUTO_ENTRY_TOUCH_MOVE_SENT`
- `RESET_GAME_INPUT_CONTROL_SENT`（code 为实际 mask）
- 既有 `GAMEPAD_LIST_ANNOUNCED_AT_TITLE`、`INPUT_SOURCE_REDECLARED_AT_TITLE`
  继续保留。

上述事件已加入 `IsEvidenceEvent`，会写入安全日志的 kept 集合；monitor 的
metrics/names 列表也已同步，避免 128 KiB 快照和事件环形缓冲淘汰。

`app/src/app_version.hpp` 升到 `0.2.142`。

## 判定标准

在线运行必须同时满足：

1. `AUTO_ENTRY_TOUCH_Y=930` 至少出现一次，并出现 `AUTO_ENTRY_TOUCH_MOVE_SENT`；
2. `AUTO_ENTRY_TAP_SENT` 持续增长，`sdk-bridge.log` 可见 `a0=0`（Down）、
   `a0=2`（Move）、`a0=1`（Up）交替；
3. `GameEntrySignalObserved=true`，且 `GAME_SDK_WILL_ENTER_GAME` /
   `GAME_SDK_ENTER_GAME` 出现在进入之后；
4. 进入后 `GAMEPAD_REDECLARE_AFTER_GAME_ENTRY`、手柄列表、mask 12 与 pad 探针
   仍按既有路径执行。

发送计数、ACK 或 `sdk-bridge.log` 的 `out` 行本身都不是进入生效的证明；只有
游戏自身的 enter 信号和进入后的画面/探针才算。

## 对照计划

1. 第一轮：保留 `cloudgenshin-autotest.touch-first-late`，验证延迟声明 + 新
   y 循环 + Move。
2. 若仍失败：删除 `.touch-first-late`，创建
   `cloudgenshin-autotest.touch-initial-12`，用同一 NRO 跑初始 mask 12 +
   纯触摸对照。
3. 若仍失败：重抽非 mhyjx1 节点（优先 jx9），记录 pod 主机名和队列时间，排除
   单节点/pod 侧输入问题。
4. 之后再按 0.2.141 文档测试“只补手柄列表、保持 mask 8”和“只补 mask 12、
   不补列表”两个变体，确认 0.2.133/0.2.138 成功究竟来自哪条路径。

普通用户、`bt`、`zmd`、键位回写与媒体策略均不变；未提交、未推送、未发版。
