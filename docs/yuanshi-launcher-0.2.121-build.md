# 0.2.121 本地构建与验证

日期：2026-11-09（Asia/Singapore）。基于本工作区累计修改构建。
本轮没有提交、推送或创建 GitHub Release。

## 本版修了什么

前几版失效的真正原因有两个，都由用户真实存档确认：

1. 0.2.120 的默认是 Off，`cloud_get_data` 被跳过，游戏拿不到任何保存配置；
2. 0.2.120 的 Keybinds 负载只回两个映射数组，把输入模式字段排除在外，
   而“主界面控制设备自动切换”的状态记录在 `USD_<uid>` 里，`GENERAL_DATA`
   的 `enableInputHotSwap` 并不是它。

本版默认改为 Keybinds：回传两个映射数组 + 输入模式字段原值 + `USD_*` 原样保留。

## 真实存档验收

用用户 Switch 上的真实配置（164,081 字节，`cloud_local_data_193285356`）
直接调用 `PrepareNativeControllerRestore()`：

```
original_bytes=164081  reply_bytes=151391
original_envelope=GENERAL_DATA USD_0 USD_122044085
reply_envelope   =GENERAL_DATA USD_0 USD_122044085
USD_0          keys=706 isUsingJoypad=false checkedDefault=false guid_len=0   identical=1
USD_122044085  keys=706 isUsingJoypad=true  checkedDefault=true  guid_len=36  identical=1
GENERAL_DATA   fields=14 maps=2 hotswap=false KeyboardMap=0
```

- 两条 `JoystickMap`（55,920 + 978 字节，`hardwareName="XInput Gamepad"`）完整保留；
- `USD_*` 与原始字节完全一致，未做任何改写；
- 输入模式字段按游戏保存的原值透传，没有强制写入。

同一份数据上 `InspectNativeConfig()` 报告：`maps=2 keyboard=0 unknown=0`。

## 主机回归

`scripts/test-mihoyo-host.ps1` 相关目标全部通过：

- `genshin_native_config`：键位负载保留映射、排除键盘／鼠标／触屏、未知根元素保留并计数、
  输入模式字段原值透传、幂等、默认 Keybinds、开关文件解析；
- `mihoyo_sdk_large_config`：桥层回包字段与采用证据比对；
- `mihoyo_sdk_bridge`、`mihoyo_sdk_cloud_data_store`、`mihoyo_safe_event_log`、
  `mihoyo_safe_log_archive`、`mihoyo_ui_language`：回归通过。

`assert` 型测试使用 `-std=c++20 -Wall -Wextra -Werror`，脚本未定义 `NDEBUG`。

## 最终产物

最终 NRO：30379729 字节。

SHA256：`77454FC15267FCF9E5416D296F6483C42E949E53A3931A47F6C27FDD2739BACA`

- NRO0 / ASET / NACP 0x4000 校验通过；
- 16 个语言槽全部填充，标题 `yuanshi launcher`、作者 `Steve1451-dot`；
- 显示版本 `0.2.121`。

## 仍缺的证据

- 云端游戏是否采用这份配置：仍需实机会话确认。
- `USD_*` 是完整本地数据，回传会覆盖云端已有进度。只适合单设备使用；
  多设备交替玩需要注意。
- 完整回传 A/B（`CGNX_NATIVE_CONFIG_RESTORE_V1`）未复测。

安装包不含账号、运行日志、登录库、SDK 用户配置、密钥、二维码或测试标记。
