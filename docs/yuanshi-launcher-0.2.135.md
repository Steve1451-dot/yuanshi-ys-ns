# 0.2.135 延后手柄声明实验

日期：2026-11-12

## 目的

0.2.134 的四轮 soak（jx12、mhyjx1、qsjh1）都停在标题页：触摸 60 次没有进入，
`GameEntrySignalObserved=false`，而 0.2.133 在 jx9 上一次成功。对比两轮时间线，
唯一能同时解释成功与失败的是输入源声明时机：

- jx9：标题页约 55 秒出现，第一次触摸早于 57 秒的 `GAMEPAD_LIST_ANNOUNCED` /
  `INPUT_SOURCE_DECLARED`，触摸被接受。
- 慢 pod：标题页约 90–110 秒才出现，此时 pod 早已收到“触摸 + 手柄”声明，
  可能已切到手柄输入模式，后续触摸被忽略。

本轮把“虚拟手柄列表 + 输入源声明”延后到游戏自己的进入信号
（`GAME_SDK_WILL_ENTER_GAME` / `GAME_SDK_ENTER_GAME`，即 `launcher_entered_`）
之后再发，验证标题页触摸能否恢复。改动只由测试标记启用，普通用户行为不变。

## 改动

- `app/src/cloudgenshin_stream_view.cpp`：
  - 新增 `defer_pad_until_entry`，仅在 `automated_seconds_>0` 且
    `cloudgenshin-autotest.defer-pad` 存在时启用。
  - `GAMEPAD_LIST_ANNOUNCED` 条件追加
    `(!defer_pad_until_entry || launcher_entered_)`。
  - `INPUT_SOURCE_DECLARED` 条件追加
    `(!defer_pad_until_entry || launcher_entered_)`。
- `app/src/app_version.hpp`：`0.2.135`。

进入游戏后仍走原有 `GAMEPAD_REDECLARE_AFTER_GAME_ENTRY` 路径：输入通道重置会
清掉两个“已发送”标志，随后重新发送手柄列表和输入源声明，手柄探针也只在
`launcher_entered_` 后 arm，因此延后不会让手柄永久缺席。

## 产物

`build/switch/yuanshi-launcher.nro`：30,387,921 字节，SHA-256
`608396EFAEEB7042496D15FCAA42E6762E2FE81F9EDE0D5330C9C97345DD176D`。
已部署到 yuzu apphome，部署后哈希一致；已写入
`cloudgenshin-autotest.defer-pad`（内容 `CGNX_DEFER_PAD_V1`）。

## soak 结果

`reports/report-0135-soak-defer/status.json`：pod jx9，`ElapsedSeconds=200`，
`Outcome=TestEndedNotGameplayProof`，`GameEntrySignalObserved=false`。

| 项 | 值 |
|---|---|
| AUTO_ENTRY_TAP_SENT | 29 |
| INPUT_SOURCE_DECLARED | 不存在（延后生效） |
| GAMEPAD_LIST_ANNOUNCED | 不存在（延后生效） |
| VIDEO_DECODED_FRAMES | 5,476 |
| VIDEO_DECODE_ERRORS | 0 |
| AUDIO_SAMPLES_PLAYED | 8,924,160 |
| AUDIO_OUTPUT_ERRORS | 0 |
| `out rtc move` | 0 |

帧目视：`frame_dump_0`（约 57 秒）仍是 miHoYo logo；`frame_dump_6`（约 117 秒）
和 `frame_dump_11`（约 167 秒）都是完整标题页，“点击进入”条位于 y≈0.947。
29 次触摸（0.5, 0.947）全部没有触发进入。

会话在 200 秒被 pod 主动关闭：`STREAM_WORKER_FINISHED` 后
`LIBPEER peer_close state=4`，随后 `AUTOTEST_FINISHED_NOT_GAMEPLAY_PROOF`。
0.2.134 的慢 pod 在同样停在标题页的情况下能保持 602 秒，差别是本轮没有发送
手柄列表和输入源声明。

## 结论

延后声明没有恢复触摸，反而说明 pod 需要输入源声明：没有声明时触摸同样无效，
而且会话可能更早被关闭。原假设“声明把手柄模式锁死、触摸被忽略”不成立。
下一步改为保留手柄列表和首次声明，但在进入游戏前每 4 秒重发一次声明，
覆盖慢 pod 输入适配器尚未就绪、首次声明被丢弃的情况。
