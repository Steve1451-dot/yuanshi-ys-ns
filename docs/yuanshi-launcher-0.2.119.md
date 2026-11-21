# 圆石启动器 0.2.119 alpha：ys 默认只回传原生手柄键位

> **已被 0.2.120 取代。** 本版有两个问题，见
> [0.2.120 变更记录](yuanshi-launcher-0.2.120.md)：
> 1. “0.2.118 run2 整场没有请求配置”是日志截断导致的误读，实际游戏请求了配置；
> 2. 默认改成 Keybinds 缺少实机依据，且负载里伪改了输入模式开关。
> 0.2.120 已把默认恢复为 Off，并把键位负载改为最小 M0。

## 背景与上一版结论

0.2.118 的默认路径是“只保存、不回传”，2026-11-09 两次实机确认：
已有保存配置时实体手柄正常，但第二次会话的游戏**根本没有请求** `cloud_get_data`，
因此键位恢复逻辑一次都没有执行。该结果证明了“不回传不会破坏手柄”，
但没有证明“回传键位能让键位跨会话保留”。

0.2.116／0.2.117 的失败则来自另一个方向：回传整份 `GENERAL_DATA` 时，
保存配置里带着上次的键盘映射和输入模式开关，游戏照单全收，于是又进入键鼠界面；
单独把 `enableInputHotSwap` 改成 `true` 也没有压住。

## 本版改动

- 默认路径改为**只回传原生手柄键位**（`NativeConfigRestoreMode::Keybinds`）：
  - 保留 `_overrideControllerMapKeyList` / `_overrideControllerMapValueList` 中
    的手柄映射，控制器 ID、顺序和其他设置逐字节不动；
  - 过滤 `KeyboardMap` 条目；
  - 仅对已存在的布尔字段 `enableInputHotSwap`、`rewiredEnableKeyboard`、
    `rewiredDisableKeyboard` 写入手柄安全值，不凭空创建字段；
  - 解析失败、没有可保留的手柄映射或超出预算时返回空字符串，
    **绝不回退整份原始配置**。
- 旧行为保留为显式 A/B：
  - 没有 `cloudgenshin-native-config.txt`：键位模式（本版默认）；
  - 空文件或 `CGNX_NATIVE_CONFIG_RESTORE_OFF_V1`：只保存、不回传；
  - `CGNX_NATIVE_KEYBINDS_RESTORE_V1`：显式键位模式；
  - `CGNX_NATIVE_CONFIG_RESTORE_V1`：完整配置回传 + `enableInputHotSwap=true`；
  - 未知内容按关闭处理。
- 新增固定安全日志，便于确认实际路径：
  - `GAME_SDK_NATIVE_CONFIG_KEYBINDS_ON`：键位模式已启用；
  - `GAME_SDK_NATIVE_SAVED_CONFIG_PRESENT`：SD 卡中已有可解析配置；
  - `GAME_SDK_NATIVE_SAVED_KEYBIND_MAPS` / `..._KEYBOARD_MAPS`：
    保存配置中的手柄／键盘映射数量；
  - `GAME_SDK_NATIVE_SAVED_HOTSWAP` / `..._KEYBOARD_ENABLED` /
    `..._KEYBOARD_DISABLED`：保存配置中的输入模式开关原值；
  - `GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT`：已发送过滤后的键位配置；
  - `GAME_SDK_NATIVE_KEYBIND_RESTORE_SKIPPED`：没有可回传的手柄键位；
  - `GAME_SDK_NATIVE_KEYBOARD_MAP_COUNT` / `..._INPUT_MODE_FIELDS_CHANGED`：
    本次过滤与改写的数量。
- SD 卡上的加密原文件只读入内存用于合并，不删除、不改写格式；
  游戏通过 `cloud_set_data` 写入的数据仍照常保存。

## 尚待实机证明

本版只完成代码与主机单测。要宣称“键位跨会话保留且保持手柄模式”，
至少需要一次满足以下条件的实机会话：

1. 日志出现 `GAME_SDK_NATIVE_CONFIG_KEYBINDS_ON`；
2. 游戏确实请求 `cloud_get_data`（出现 `GAME_SDK_CUSTOM_REQUEST` /
   `GAME_SDK_CLOUD_DATA_READ_HIT`）；
3. 出现 `GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT`，且
   `GAME_SDK_NATIVE_KEYBOARD_MAP_COUNT` / `..._INPUT_MODE_FIELDS_CHANGED` 符合预期；
4. 进入游戏后自定义键位仍在，界面仍是手柄模式。

如果游戏整场不请求配置，本次会话不会执行任何恢复；这不等于恢复失败，
但也不能作为成功证据。若键位恢复后仍回到键盘模式，下一步应单独隔离
“映射负载本身”与“其他配置字段”的影响，不再发布。

## 验证方法

1. 安装 0.2.119，确认 `cloudgenshin-native-config.txt` 不存在（默认键位模式）。
2. 进入 ys，走到游戏会读取云端配置的界面；观察手柄是否可用。
3. 返回后提供 `cloudgenshin-safe.log`（或完整 `stream-safe-*.log`），重点看上述事件。
4. 对比项：把开关文件写成空文件后再进一次，确认只保存路径仍保持手柄正常。

日志只能证明本机发送与过滤结果；键位是否被游戏采用、输入模式是否保持，
以实机画面和操作结果为准。

## 原生键位保存与恢复边界

1. 用户在游戏内修改原生手柄键位。
2. 游戏通过 SDK `cloud_set_data` 提交配置；启动器在 SD 卡专用目录加密、原子保存。
3. 下次进入时，启动器只把过滤后的手柄映射还回去，并固定输入模式开关。
4. 不修改游戏原生键位表、控制器 ID、灵敏度或任何本地改键；不把手柄转换成键盘。

配置格式可对照开源
[Collapse GeneralData 定义](https://github.com/CollapseLauncher/Collapse/blob/main/CollapseLauncher/Classes/GameManagement/GameSettings/Genshin/RegistryClass/GeneralData.cs)。
该来源不证明云端游戏采用哪个输入模式；三个输入模式字段名也尚未在用户真实存档上逐一核对。
