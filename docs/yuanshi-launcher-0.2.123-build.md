# 0.2.123 构建与验证

日期：2026-11-09（Asia/Singapore）。本轮没有提交、推送或创建 Release。

## 变更动机

0.2.121/0.2.122 实机日志显示，终端 10（CTT_WEB_KEYBOARD，PC 网页）下：

- 启动页发的是鼠标（`LAUNCHER_POINTER_MOUSE`），不是触摸；
- 鼠标事件会让云端把输入设备切成键鼠（`RESET_GAME_INPUT_CONTROL_SENT code=4`）；
- 之后手柄帧仍在发送，游戏不再响应。

用户提出直接模拟手机：触屏与手柄并用，并由游戏内
「主界面控制设备自动切换」完成热插拔。这与终端族行为一致。

## 终端族对照

| 终端 | 家族 | 启动页输入 | cps |
| --- | --- | --- | --- |
| 3 | CTT_ANDROID | 触摸 | `android_mihoyo` |
| 10 | CTT_WEB_KEYBOARD | 鼠标 | `pc_mihoyo` |

`UsesTouchLauncher()` 对 2/3/6/7/9 返回真，使用官方触摸流。

## 本版改动

1. 默认终端 10 → 3（Android）。
2. 自动进入按终端族分流：触摸族发触摸点击（`AUTO_ENTRY_TOUCH_PRESS`），
   非触摸族发虚拟手柄 A（`AUTO_ENTRY_PAD_PRESS`）。
3. 物理面键不再转换成指针点击（0.2.122 已移除）。
4. 触摸与手柄帧同时发送，互不排斥。
5. Keybinds 回包保持 4 字段：两个映射数组 +
   `enableInputHotSwap` + `disableFallbackControllerType`；`USD_*` 原样保留。

## 真实存档验收

用户 Switch 上的真实配置（164,081 字节，`cloud_local_data_193285356`）：

```
original_bytes=164081  reply_bytes=148918
reply_envelope = GENERAL_DATA + USD_0 + USD_122044085
USD_0          isUsingJoypad=false checkedDefault=false guid=0   identical=1
USD_122044085  isUsingJoypad=true  checkedDefault=true  guid=36  identical=1
GENERAL_DATA   fields=4  maps=2  KeyboardMap=0
```

## 主机回归

`genshin_native_config`、`mihoyo_sdk_large_config`、`mihoyo_ui_language`、
`mihoyo_stream_input_policy`、`mihoyo_launcher_input_policy`、
`mihoyo_game_control` 全部通过。

## 最终产物

最终 NRO：30379729 字节。

SHA256：`5FAF232B554228E21683E00C6B99F59E3C2B110AC9756B78E72DAC5314BC60BA`

- NRO0 / ASET / NACP 0x4000 校验通过；
- 16 个语言槽全部填充，标题 `yuanshi launcher`、作者 `Steve1451-dot`；
- 显示版本 `0.2.123`。

## 仍缺的实机证据

- 终端 3 下启动页是否接受触摸（应为 `LAUNCHER_POINTER_TOUCH`、
  `AUTO_ENTRY_TOUCH_PRESS`）。
- 进入游戏后手柄是否持续有效。
- 游戏内「控制设备」是否显示手柄，触屏与手柄能否自动切换。
- 键位是否在下次进入时仍在。

安装包不含账号、运行日志、登录库、SDK 用户配置、密钥、二维码或测试标记。
