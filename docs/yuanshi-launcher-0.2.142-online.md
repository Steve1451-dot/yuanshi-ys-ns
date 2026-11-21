# 0.2.142 在线对照记录

日期：2026-11-14（Asia/Singapore）。排障 alpha，不是验收结论。

## 第一轮：延迟声明 + 新坐标 + Down→Move→Up

`reports/report-0142-soak-late-move/status.json`（1,253 秒，`TestEndedNotGameplayProof`）：

| 项 | 值 |
|---|---|
| `AUTO_ENTRY_TOUCH_Y` | 930 起，随后循环 947 / 915 / 960 |
| `AUTO_ENTRY_TOUCH_MOVE_SENT` | 59 |
| `AUTO_ENTRY_TAP_SENT` | 60（预算用尽） |
| `INPUT_SOURCE_DECLARED` | 8（709s 前） |
| `GAMEPAD_LIST_ANNOUNCED_AT_TITLE` | 1118 / 2835（709s） |
| `INPUT_SOURCE_REDECLARED_AT_TITLE` | 12（709s） |
| `RESET_GAME_INPUT_CONTROL_SENT` | 12 |
| `GameEntrySignalObserved` | false |
| `VIDEO_DECODED_FRAMES` / `DECODE_ERRORS` | 17,727 / 0 |

`sdk-bridge.log` 首次触摸在 +55.9 秒，`a0=0`（Down）、`a0=2`（Move）、`a0=1`（Up）
严格交替，共 32 条 trace（trace 上限）。`frame_dump_11`（约 1,000 秒）仍是标题页，
页脚 `CNRELWin7.2.0_R49411677_S4929486_D49419668`。

结论：坐标、Move、延迟 700 ms 三个假设全部排除。触摸确实按官方序列发出，pod
没有让游戏进入。

## 终端类型与 pod 实际运行的游戏

`cloudgenshin-terminal.txt` / 默认的 `3`（`CTT_ANDROID`）是**客户端身份**，写进
`StartGameReq` field 4，告诉云服务“我是安卓客户端”。它不是“pod 上跑安卓游戏”。

这个项目模拟的是官方 **Android APK 这个瘦客户端**：它只负责拉流、发 RTC 输入。
真正的游戏实例跑在机房，而云原神给所有终端串流的是 **Windows/PC 版游戏**——标题页
页脚 `CNRELWin7.2.0_...`（`CN`/`REL`/`Win`）就是 PC 构建的版本串，安卓版不会出现
这个格式。所以“模拟安卓客户端 + pod 上是 Windows 游戏”两者并不矛盾。

含义：

- 我们的触摸必须由 pod 侧的适配层翻译成 Windows 侧的输入才可能生效；
- 终端类型影响 pod 对输入的解释（触摸 / 鼠标 / 手柄），也影响它给哪套启动器 UI；
- 0.2.43 用终端 5（WebPC）+ 鼠标点击曾把点击送进游戏（2 秒后出弹窗），
  说明同一条链路在鼠标帧上是通的。

`R49411677` 只是告诉我客户端补丁号变了，不能单独证明失败原因；0.2.133/0.2.138
成功时页脚同样是 `CNRELWin*`。

## 新证据：触摸停止后 pod 上报空闲

同一轮在 1,243 秒和 1,253 秒出现两次 `GAME_SDK_NO_ACTION_TIP`（cmd 20014
`RtcNotPlayingTips`）。需要谨慎解读：自动进入的 60 次触摸在约 1,002 秒用尽，
之后客户端不再发任何输入，因此这条提示**与“触摸停止”一致**，不能单独证明
pod 没有把触摸当成用户操作。`GAME_SDK_KEEP_PLAYING_SENT` 是 `EmitOnce` 事件，
只记录第一次，也说明不了后续刷新。

能确定的只有：触摸帧已按官方序列发出，pod 没有让游戏进入；节点为
`qsjh1-c1-10037-2`（`cg-qsjh1-wss-mobile-...`），不是 0.2.133 成功的 jx9。

## 客户端已更新这一事实

0.2.133 / 0.2.138 成功时页脚同样是 `CNRELWin*`，只是补丁号较早
（`R49086080`）。它们证明“安卓客户端 + Windows 游戏 + 触摸”曾经能进入，所以
“Windows 页面不认触摸”不是先天限制，只可能是这次 pod 或这版补丁的输入处理变了。

## 第二轮（未完成）：初始 mask 12 对照

第一次 `report-0142-touch-initial-12` 使用 `.touch-initial-12`，但队列在 831 秒
仍停在 43 位，手动停止，没有进入串流，无输入证据。第二次同样在队列阶段停止。

初始 mask 12 对照因此还没有有效结果，不能据此判断 0.2.133/0.2.138 的成功路径。

## 新增键盘对照（测试标记）

`app/src/cloudgenshin_stream_view.cpp`：

- 新增测试标记 `cloudgenshin-autotest.key-title-probe`
  （内容 `CGNX_KEY_TITLE_PROBE_V1`）。
- 存在时，在触摸预算用尽前（第 59 次之后）发一次 Space 按下/抬起，并记录
  `AUTO_ENTRY_KEYBOARD_PROBE_SENT`。pod 跑的是 Windows 客户端，标题页同样接受
  Space。
- 放在预算末尾是刻意的：发送键盘帧可能让 pod 切到键盘模式，这样前面 59 次
  触摸仍是干净的对照。

判定：

- Space 能进入 → pod 输入通道正常，只有触摸路径有问题（终端类型/触摸映射）；
- Space 也进不去 → pod 侧输入没有落地，问题不在触摸坐标或声明顺序。

该二进制与 0.2.142 其余代码相同，版本号仍是 `0.2.142`，
SHA-256 `D45EFDB83881C2D99F06FFAB129B7BD31D5044BB4CD89B4A941295882D1F2A78`，
已部署到 yuzu apphome。标记存在时才有行为差异，普通用户路径不变。

普通用户、`bt`、`zmd`、键位回写与媒体策略均不变；未提交、未推送、未发版。
