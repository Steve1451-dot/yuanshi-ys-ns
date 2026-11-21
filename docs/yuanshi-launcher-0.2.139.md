# 0.2.139 原生手柄对照诊断

日期：2026-11-14（Asia/Singapore）。排障 alpha，不代表手柄或重启保留已验收。

## 当前证据

最终 0.2.138（SHA256 `D6C2FC3E10B9C500B5CC51B4EBC20701C09EFD901692C8B05CB89D703858C159`）
在本日两轮 yuzu 真实会话中恢复了已有登录、通过排队并正常出图，但测试截止仍停在启动页。
默认 Keybinds 轮末快照 295 秒、解码 6,861 帧；Exact 轮末快照 297 秒、解码 4,491 帧。
Exact 轮排队占用了约 140 秒，不能与前轮当作等长串流性能对比。

`reports/report-0138-keybind-input-trace-nov14/status.json` 与
`reports/report-0138-exact-input-trace-nov14/status.json` 的
`GameEntrySignalObserved=false`。最终输入日志可见 A 的 `buttons=4096` 与中性松开，
说明按压经过本地合成并交给 RTC；不证明云端应用收到了或响应了它。

## 改动

- 手柄变化边沿使用独立额度：启动阶段最多 32 条，首次游戏进入信号后再保留最多 32 条。
  重复的 `will_enter_game/enter_game` 不会反复重置额度；触摸等输入仍共用原来的 32 条。
  仅启用测试 trace 时落盘，普通用户没有逐帧日志，也不改变输入报文。
- 增加测试专用的 Xbox 360 型号对照。只有 ys、自动测试开启且已有有效 pad 探针标记时，
  `cloudgenshin-autotest.pad-identity` 中的精确标记 `CGNX_XBOX360_PROBE_V1`
  才会将 PID 改为 `0x028e`。否则沿用 `0x045e/0x0b13`。支持无换行、LF、CRLF，
  不接受未知内容或任意 VID/PID。普通启动、bt、zmd 不受影响。
- 修正错误注释：安卓 `GamepadManager.isGamepadOnAllowList` 是本地枚举过滤，
  不能据此断言云端驱动一定拒绝 Xbox 360。`ClientCore.mockXboxVid/Pid` 也没有
  把 Microsoft 型号统一改写为某一个 PID。型号作用仍需真实按键效果对照确认。

这不是恢复启动器本地改键，也没有修改游戏原始键位或默认配置恢复策略。
不更改认证、TLS、媒体路径、终端类型或其他游戏。

## APK 核对记录

本次直接核对已有 APK 7.1.0 的本地反编译/ARM64 代码，没有要求新 APK：

- `ClientCore.java:208–219,606–621`：Microsoft VID/PID 在 `mockXboxVid/Pid`
  中保持原值；不能从 Java 扫描白名单推导云端驱动能力。
- `libclient_core_impl.so` 的 `onGamepadPressed`（0x85ab0）及其 worker 回调
  （0x8ab84–0x8ac78）：回调在本地状态检查后把已构造的 `RtcDataChannel`
  交给 `sendRtcData`；这里未发现补写 field 17 的过程。不再猜测定期 type-31 能激活手柄。
- `TouchableSurfaceView.java`：单指按下/松开为 0/1，坐标除以视图宽高，主指 ID
  来自第一个接触点。与现有单指输入的归一化和主指规则一致。
- `libtransport_engine.so` 的同名远端数据通道处理（0x247104 起）会接管
  `webrtc_data_channel`。因此“必须另开本地 SID 0，远端 SID 1 一定不能发送”
  也不是已证实的结论，本版没有盲目改通道或复制输入。

这些是范围有限的静态核对，不是端到端手柄验收；后续仍以真实画面响应为准。

## 验证

主机测试 `mihoyo_autotest_pad_policy`、`mihoyo_input_packet`、`mihoyo_rtc_session` 通过。
覆盖型号标记的范围与格式、标题阶段日志耗尽后仍记录右摇杆、重复进入信号不扩大日志额度、
输入松开/失败重试和原有报文。Switch 构建及后续 yuzu 对照结果另记构建报告。

测试使用本地加密游戏配置基线，每轮结束再恢复校验；不输出登录令牌、Cookie、二维码，
不把运行时配置或截图加入发布包。
