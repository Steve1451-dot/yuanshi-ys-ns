# 0.2.134 第二次 Plus 探针与两轮 soak 记录

日期：2026-11-11。

## 目的

0.2.133 的 600 秒 soak 已经证明：

- 右摇杆在真实游戏世界生效（`pad_probe_1→2` 全屏平均像素差 13.22，镜头明显右转）。
- 键位链完整（保存 3 张映射 → 回给游戏 → 游戏写回 3 张，`MATCH_REPLY=1`）。
- 全程没有鼠标指针输入（`out rtc move` 为 0）。

但第一次 Plus（进入后 20.0–20.7 秒）没有在探针帧里打开菜单。0.2.133 的进入
信号在 t=60 秒，探针帧在 77–88 秒；第一次 Plus 落在进入世界后的“推荐任务”
提示期间，UI 可能吞掉了 Start。0.2.134 只改探针时序，在进入后 **+45 秒**再按
一次 Plus（按住 700 ms），产品输入路径不变。

## 改动

- `app/src/mihoyo/autotest_pad_policy.hpp`：新增 phase 7/8，elapsed 45.0–45.7 秒
  按住 Plus，45.7 秒后释放；phase 6 延长到 45 秒。
- `tests/mihoyo_autotest_pad_policy_test.cpp`：新增第二次 Plus 的断言
  （`sample(45999)` phase 6、`sample(46000)` phase 7 且按住、`sample(46700)`
  phase 8 且释放）。注意 `arm(1000)` 后 `sample()` 的参数是绝对毫秒。
- `tests/input_transport_policy_test.cpp`：0.2.131 把手柄身份改成
  `0x045e/0x0b13` 后，这里仍硬编码旧的 `0x028e`，导致整轮主机测试失败。
  改为引用 `input::kXboxVendorId` / `input::kXboxProductId`，与产品代码同源。
- `scripts/cloudgenshin-monitor.ps1`：新增输入/键位/自动进入 counters，
  包括 `INPUT_SOURCE_DECLARED`、`GAMEPAD_LIST_ANNOUNCED`、
  `GAME_SDK_NATIVE_*`、`AUTO_ENTRY_*`。
- `app/src/app_version.hpp`：`0.2.134`。

## 产物

`build/switch/yuanshi-launcher.nro`，30,387,921 字节，
SHA-256 `24D7C025153AAACEA937F1580EFB956AB5E45AD8EFB13D973589C7132A44B207`。
已部署到 yuzu apphome，部署后哈希一致。

## 主机测试

完整 `scripts/test-mihoyo-host.ps1`：**全部 PASS**（2026-11-11 18:42 结束），
包括 `audio_pipeline/ssrc`、`input_transport_policy`、`mihoyo_input_packet`、
`mihoyo_autotest_pad_policy`。

本轮修掉两个测试遗留：

- `input_transport_policy`：0.2.131 把手柄身份改成 `0x045e/0x0b13` 后，测试仍
  硬编码旧的 `0x028e`。改为引用 `input::kXboxVendorId` / `kXboxProductId`，
  与产品代码同源。
- `mihoyo_input_packet`：0.2.131 的允许列表断言写成 `input::kXboxProductId`，
  但文件顶部已有 `using namespace cloudgenshin::input;`，应使用不带前缀的
  `kXboxProductId`。之前被 `input_transport_policy` 的失败挡在后面，整轮测试
  没跑到这里。

## soak 结果

### 第一轮 `reports/report-0134-soak-a.json`

pod jx12，602 秒，`Outcome=TestEndedNotGameplayProof`，`Failed=false`。

| 项 | 值 |
|---|---|
| GameEntrySignalObserved | `false` |
| Stage | `GameSdkLogin` |
| AUTO_ENTRY_TAP_SENT | 60（预算用尽） |
| INPUT_SOURCE_DECLARED | 12（触摸 + 手柄） |
| GAME_CONFIG_XBOX / PS | 1 / 1 |
| GAMEPAD_VENDOR_ID / PRODUCT_ID | 1118 / 2835 |
| VIDEO_DECODED_FRAMES | 17,679 |
| VIDEO_DECODE_ERRORS | 0 |
| AUDIO_SAMPLES_PLAYED | 28,394,880 |
| AUDIO_OUTPUT_ERRORS | 0 |
| `out rtc move` | 0 |

键位链仍完整：

```
t=2s   GAME_SDK_NATIVE_SAVED_KEYBIND_MAPS        3
t=10s  GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT
t=17s  GAME_SDK_NATIVE_WRITTEN_KEYBIND_MAPS      3
t=17s  GAME_SDK_NATIVE_WRITTEN_MAPS_MATCH_REPLY  1
```

帧转 PNG 目视：`frame_dump_0` 为 miHoYo logo，`frame_dump_11`（约 106 秒）仍是
标题页“点击进入”。60 次触摸（0.5, 0.947）没有触发进入。这一轮没有进入游戏，
因此探针没有 arm，`pad_probe_*` 不存在。属于 pod 实例/标题页输入问题，不是
0.2.134 探针改动导致。

### 第二轮 `reports/report-0134-soak2-dispatch-error.json`

启动 31 秒即 `DISPATCH_NETWORK_ERROR`：`HTTP_FAILURE_PHASE=5`、
`HTTP_FAILURE_TOTAL_MS=30004`，DNS/连接/TLS 均正常（8/22/36 ms），是等待响应
超时的瞬时网络故障，未到游戏阶段。归档后重跑。

### 第三轮 `reports/report-0134-soak3/status.json`

pod `mhyjx1-c1-675-3`，602 秒，`Outcome=TestEndedNotGameplayProof`，
`Failed=false`。

| 项 | 值 |
|---|---|
| GameEntrySignalObserved | `false` |
| Stage | `GameSdkLogin` |
| AUTO_ENTRY_TAP_SENT | 60（预算用尽） |
| INPUT_SOURCE_DECLARED | 12（触摸 + 手柄） |
| GAME_CONFIG_XBOX / PS | 1 / 1 |
| GAMEPAD_VENDOR_ID / PRODUCT_ID | 1118 / 2835 |
| VIDEO_DECODED_FRAMES | 17,631 |
| VIDEO_DECODE_ERRORS | 0 |
| AUDIO_SAMPLES_PLAYED | 28,341,208 |
| AUDIO_OUTPUT_ERRORS | 0 |
| `out rtc move` / `btn` / `key` | 0 / 0 / 0 |

键位链同样完整：

```
t=2s   GAME_SDK_NATIVE_SAVED_KEYBIND_MAPS        3
t=10s  GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT
t=17s  GAME_SDK_NATIVE_WRITTEN_KEYBIND_MAPS      3
t=17s  GAME_SDK_NATIVE_WRITTEN_MAPS_MATCH_REPLY  1
```

触摸实际在发：`sdk-bridge.log` 首条 `out rtc touch` 在 +55.2 秒，trace
上限 32 条（16 次点击）最后一条在 +128.3 秒；自动进入计数到 407 秒才达到
60 次。`frame_dump_9`（约 90 秒）和 `frame_dump_11`（约 110 秒）都是标题页
“点击进入”，与 0.2.133 的标题页布局、页脚版本
`CNRELWin7.2.0_R49086080_S49291486_D49291486` 完全一致；两帧平均像素差
4.18 / 4.31，说明画面在动，不是卡死。

结论：三轮 0.2.134 分别落在 jx12、mhyjx1 两个 pod 上，都是标题页不响应
触摸；0.2.133 在 jx9 上同一坐标（0.5, 0.947）一次点击即进入。0.2.134
本次没有改动自动进入/触摸发送逻辑，改动集中在探针时序、版本号和测试脚本，
因此按 pod 侧标题页输入问题记录，待真机复核。0.2.133 的真实游戏画面与
摇杆证据仍然有效；Plus 菜单留待真机确认。

## 尚待真机验证

- 重启后键位是否仍在。
- 触摸 + 手柄同时使用、热插拔。
- Plus 菜单手感。
- 真实网络下的长时间稳定性。

## 文件

- `app/src/mihoyo/autotest_pad_policy.hpp`
- `tests/mihoyo_autotest_pad_policy_test.cpp`
- `tests/input_transport_policy_test.cpp`
- `scripts/cloudgenshin-monitor.ps1`
- `reports/report-0134-soak-a.json`
- `reports/report-0134-soak2-dispatch-error.json`
