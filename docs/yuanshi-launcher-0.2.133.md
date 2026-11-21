# 0.2.133 自动进入预算延长与真实游戏画面验证

日期：2026-11-11。

## r0132-1 根因纠正

0.2.132 的两轮 yuzu 运行都在约 391 秒以 `GAME_SDK_SESSION_ENDED` 结束，
`AUTOTEST_PAD_PROBE_PHASE=0`、`GameEntrySignalObserved=false`。当时的判断是
`StartGameRsp` 缺失、输入通道没建好，准备做“本地默认兜底”。

复核 `sdk-bridge.log` 后否定了这个判断：

```
[+1343ms] dc open sid=1 ppid=50 label=webrtc_data_channel
[+1362ms] out rtc hb id=1 ts=...
```

数据通道在 **+1.343 秒**已打开，心跳在 **+1.362 秒**已发出，说明认证、peer、
输入通道早已就绪。安全日志里的 `GAME_CONFIG_* at=56` 只是监控第一次采样到该
计数的时间，不是认证时间。

真正阻塞是自动进入的点击预算：慢启动 pod 在 55–100 秒仍停在加载画面，10 次
自动点击全部花在那里，标题页约 116 秒才出现。此时探针已经错过窗口。

因此**没有**实施“假认证兜底”：在未授权时发输入会在真实登录/排队页误触，
风险高于收益。

## 改动

`app/src/cloudgenshin_stream_view.cpp`：

- 新增 `kAutoTestAutoEntryMaxTaps=60`，只在 `cloudgenshin-autotest.pad` 存在时
  生效；普通用户仍是 `kAutoEntryMaxTaps=10`。
- autotest 下不再因亮度变化提前结束进入序列；普通用户仍保留亮度保护。
- 游戏自身的进入信号（`GAME_SDK_WILL_ENTER_GAME` / `GAME_SDK_ENTER_GAME`）
  仍会立即停止序列，探针只在这之后才 arm。

`app/src/app_version.hpp` 升到 `0.2.133`。

## 证据

产物：`build/switch/yuanshi-launcher.nro`，30,387,921 字节，
SHA-256 `4F8B97C5358C6B97C5D7C691DE0FEDA7AD864D5561577927FCF61FC0B86CF0E7`。

`reports/report-0133-soak/status.json`（600 秒 soak）：

| 项 | 值 |
|---|---|
| Outcome | `TestEndedNotGameplayProof` |
| Failed | `false` |
| ElapsedSeconds | 602 |
| GameEntrySignalObserved | `true` |
| AUTOTEST_PAD_PROBE_PHASE | 6 |
| VIDEO_DECODED_FRAMES | 17,090 |
| VIDEO_DECODE_ERRORS | 0 |
| AUDIO_SAMPLES_PLAYED | 28,183,515 |
| AUDIO_OUTPUT_ERRORS | 0 |
| GAME_CONFIG_XBOX | 1 |
| GAME_CONFIG_PS | 1 |
| GAMEPAD_VENDOR_ID / PRODUCT_ID | 1118 / 2835 |

`cloudgenshin-safe.log` 的键位链（本轮完整走通）：

```
t=2s   GAME_SDK_NATIVE_SAVED_KEYBIND_MAPS        3
t=14s  GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT
t=22s  GAME_SDK_NATIVE_WRITTEN_KEYBIND_MAPS      3
t=22s  GAME_SDK_NATIVE_WRITTEN_MAPS_MATCH_REPLY  1
```

游戏写回的 3 张手柄映射与启动器回给它的内容一致（`MATCH_REPLY=1`），这是
“游戏实际采用”的证据，而不只是发送成功。

`sdk-bridge.log`：

```
out rtc touch n=1 f0=0 a0=0   (+55202ms)
out rtc touch n=1 f0=0 a0=1   (+55320ms)
```

整轮没有 `out rtc move`（鼠标指针）事件——启动器只用触摸/手柄，符合
“绝不发送鼠标指针以免 pod 切回键盘模式”的要求。

## 真实游戏画面

探针帧为 960×540 8-bit 灰度 BMP，转 PNG 后目视确认：

- `pad_probe_1`：角色站在蒙德城广场，完整游戏 HUD、队伍栏、任务栏、UID。
- `pad_probe_1 → 2` 全屏平均像素差 **13.22**，镜头明显右转；这是右摇杆
  在真实游戏世界里生效的直接证据。
- `pad_probe_2 → 3` 差 11.53（“受到称赞”提示消失、任务提示出现）。
- `pad_probe_3 → 4` 差 3.6，`pad_probe_4/5/6` 均为正常游戏画面，Plus 菜单
  没有出现在帧里。原因见 0.2.134：第一次 Plus 落在进入世界后的“推荐任务”
  提示期间，UI 可能吞掉了 Start；0.2.134 在 +45 秒补了第二次 Plus。

## 尚待真机验证

yuzu 只能证明协议与输入链路，以下必须在 Switch 真机确认：

- 重启后键位是否仍在（本次只证明游戏写回一致）。
- 触摸 + 手柄同时使用、热插拔。
- Plus 菜单手感（0.2.134 补测）。
- 真实网络下的长时间稳定性。

## 文件

- `app/src/cloudgenshin_stream_view.cpp`：autotest 进入预算与亮度门控
- `app/src/app_version.hpp`：0.2.133
- `reports/report-0133-soak/status.json`：soak 证据
