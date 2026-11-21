# 0.2.143 标题页三种入口设备轮换

日期：2026-11-14（Asia/Singapore）。排障 alpha，不代表已验收。

> 历史实验，已在 0.2.144 退役。离线复查发现 B 探针的第一帧虽为 XInput B，
> 后续连续保持帧却被 `ComposeGamepadFrame` 改成 A；因此不能据此声称已有效比较 A/B。
> 本文下方“三者无效即 pod/版本原因”的推断不成立，必须先排除本地时序和传输等问题。
> 不再启用这条轮换路线，旧标记不再改变 0.2.144 的行为。

## 目的

从 0.2.140 起，标题页只用触摸，连续 4 轮（mhyjx1 / qsjh1）60 次触摸全部无效，
而 0.2.133 在 jx9 上同一个坐标一次就进入。pod 上的标题页是 **PC 客户端页面**
（页脚 `CNRELWin*`），说明这是 Windows 游戏的启动器 UI，它同时接受鼠标点击、
XInput A 和 XInput B。继续把 60 次预算全花在触摸上，等于只测一种设备。

本轮把自动进入改成三设备轮换，一次运行同时覆盖三种入口。

## 改动

`app/src/mihoyo/launcher_input_policy.hpp`：

```cpp
enum class LauncherEntryDevice {Touch,PadA,PadB};
LauncherEntryDevice LauncherEntryDeviceFor(bool mix_enabled,unsigned completed_taps);
```

- `mix_enabled=false`（默认，普通用户）→ 始终 `Touch`，行为与 0.2.142 完全一致。
- `mix_enabled=true` → 按已完成次数轮换：`0=Touch`、`1=PadA`、`2=PadB`、`3=Touch`…

`app/src/cloudgenshin_stream_view.cpp`：

- 新增标记 `cloudgenshin-autotest.entry-mix`（`CGNX_ENTRY_MIX_V1`）。
- 触摸分支仅在本次轮换到 `Touch` 时执行；否则走手柄分支。
- 手柄分支按设备选择按键：`PadA` 发 XInput A（物理 Switch B，历史路径不变），
  `PadB` 发 XInput B（物理 Switch A），并记录 `AUTO_ENTRY_PAD_B_PRESS`；
  `PadA` 仍记录既有的 `AUTO_ENTRY_PAD_PRESS`。
- 触摸分支的 Down→Move→Up、y 坐标循环、延迟声明全部沿用 0.2.142。
- 手柄按下仍保持 120 ms 后用中性帧释放，不会留下按住的键。
- 键盘 Space 对照（`cloudgenshin-autotest.key-title-probe`）保留，但本轮不启用。

`tests/mihoyo_launcher_input_policy_test.cpp` 增加轮换断言：关闭标记时全程触摸；
开启标记时 0/1/2 依次为 Touch/PadA/PadB，且 6 次内必然同时出现过 PadA 与 PadB。

## 判定

一轮即可区分：

- 若只有触摸能进入 → 触摸路径其实通，之前是 pod/节点问题；
- 若只有 XInput A 或 B 能进入 → pod 输入通，但只认手柄，需要调整进入与后续输入策略；
- 三者都进不去 → 与设备无关，是 pod 侧或客户端版本问题，需要换节点再判。

进入后仍走既有的 `GAMEPAD_REDECLARE_AFTER_GAME_ENTRY` 路径（手柄列表 + mask 12）。

## 产物

`build/switch/yuanshi-launcher.nro`，版本仍为 `0.2.142`（同一代码线的诊断变体），
SHA-256 `BF8370D9D1E4EBD126719321053BC79EA94EF0346ED1D0751C3E991C5BE7866C`，
已部署到 yuzu apphome。

普通用户、`bt`、`zmd`、键位回写与媒体策略均不变；未提交、未推送、未发版。
