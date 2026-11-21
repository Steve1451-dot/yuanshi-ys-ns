# 0.2.140 触摸优先进入实验

日期：2026-11-14（Asia/Singapore）。排障 alpha，不代表已验收。

## 目的

0.2.133 在 jx9 上成功进入，`sdk-bridge.log` 的第一次触摸是 +55.2 秒，标题页
约 55 秒出现；0.2.134 的慢 pod 标题页要到 90–110 秒，而手柄列表和
`INPUT_SOURCE_DECLARED`（mask 12）在约 57 秒就发出了。之后触摸一直无效。

0.2.135 把列表和声明都延后，结果触摸同样无效，pod 还在 200 秒关闭会话，
说明 pod 需要输入源声明才能接受触摸；但 mask 12 可能在标题页就绪前就把 pod
切到手柄模式。0.2.136 的周期重发没有成功进入的报告；0.2.137 的协议核对也确认
type-31 只是清除按住状态，不是模式切换。

本轮改为“触摸优先”：

- 进入游戏前只声明 `TOUCH_SCREEN`（mask 8），让 pod 接受标题页触摸；
- 进入游戏前不发送虚拟手柄列表，也不穿插手柄 A 对照；
- `GAME_SDK_WILL_ENTER_GAME` / `GAME_SDK_ENTER_GAME` 到达后，沿用已有的
  `GAMEPAD_REDECLARE_AFTER_GAME_ENTRY` 路径：发送手柄列表并重新声明 mask 12，
  让游戏内手柄生效。

改动仅由测试标记 `cloudgenshin-autotest.touch-first` 启用，普通用户路径不变。

## 改动

- `app/src/cloudgenshin_stream_view.cpp`：
  - 新增 `touch_first_until_entry`，仅在 `automated_seconds_>0` 且标记存在时启用。
  - 进入前 `GAMEPAD_LIST_ANNOUNCED` 和首次按键重发都被跳过。
  - 进入前 `INPUT_SOURCE_DECLARED` 使用 `kInputControlTouchScreen`（8），进入后
    恢复 `LauncherInputSource`（触摸终端为 12）。
  - 触摸优先时关闭 `TestLauncherPadFallback`，标题页阶段只发触摸。
- `app/src/app_version.hpp`：`0.2.140`。

## 产物

`build/switch/yuanshi-launcher.nro`：30,400,209 字节，SHA-256
`2E8A2D816F06917E33AF53C91B17BB417AD50428009E5A6437C7D669A43E68EB`。
已部署到 yuzu apphome，部署后哈希一致；已写入
`cloudgenshin-autotest.touch-first`（内容 `CGNX_TOUCH_FIRST_V1`），并移除
`cloudgenshin-autotest.redeclare`。

## soak 结果

### 首轮 report-0140-soak-touch-first：无效，不是方案失败

2026-11-14 11:28 启动的首轮在 t=24s 收到 `CANCEL_REQUESTED`，随后
`LOGIN_CLEARED`、QR/手机确认、`GAME_SELECTED=endfield`，报告停在
`NeedsPhoneConfirmation`、702 秒，`RenderSubmissionObserved=false`。
安全日志证明运行期间有人操作了 yuzu 窗口并切换了游戏与登录，因此这一轮没有
跑到串流，不能用来判断触摸优先方案。

### 第二轮 report-0140-soak-touch-first-2：mask 8 单独失败

2026-11-14 11:45 启动的第二轮抽到 mhyjx1，正常出图，标题页约 190 秒出现。
`INPUT_SOURCE_DECLARED=8`、没有 `GAMEPAD_LIST_ANNOUNCED`，自动触摸从 220 秒开始，
共 46 次，447 秒主动停止时 `GameEntrySignalObserved=false`。

这轮 SDK 登录在 183 秒完成，`GAME_SDK_KEEP_PLAYING_SENT` 在 220 秒，触摸并不算
“打得太早”。因此失败点收敛到：pod 在缺少虚拟手柄列表 / mask 12 时不会接受
标题页触摸。下一步做 0.2.141：标题页第一次触摸前补发列表 + mask 12。

### 隐藏窗口

yuzu 是 Qt GUI，`ProcessStartInfo.WindowStyle=Hidden` 不会隐藏主窗口。测试脚本
现在在进程窗口出现后调用 Win32 `ShowWindow(SW_HIDE)`；portable 配置已是
`pauseWhenInBackground=false`，隐藏后排队与模拟继续。普通 `-Visible` 运行不受影响。
