# 0.2.122 构建与验证

日期：2026-11-09（Asia/Singapore）。本轮没有提交、推送或创建 Release。

## 0.2.121 实机结果

用户装 0.2.121 后测试，日志出现两个独立现象。

### 回传成功

```
t=194s GAME_SDK_NATIVE_CONFIG_KEYBINDS_ON
t=194s GAME_SDK_NATIVE_CONTROLLER_MAP_COUNT code=2
t=194s GAME_SDK_NATIVE_BIND_REASON code=0
t=194s GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT
t=194s GAME_SDK_LARGE_REPLY_SENT code=223009
t=195s GAME_SDK_LARGE_REPLY_ACKNOWLEDGED
t=203s GAME_SDK_NATIVE_WRITTEN_KEYBIND_MAPS code=2
t=203s GAME_SDK_NATIVE_WRITTEN_MAPS_MATCH_REPLY code=1
```

`WRITTEN_MAPS_MATCH_REPLY=1`：游戏回写的映射与回传逐条一致，
是目前最可靠的云端采用证据。

### 输入被触摸注入打断

```
t=218s INPUT_LOCAL_PRESS_SAMPLED
t=218s PAD_A_ENTRY_TAP_SENT
t=222s RESET_GAME_INPUT_CONTROL_SENT code=4
t=222s INPUT_SOURCE_DECLARED code=4
t=241s TOUCH_SENT_DOWN / MOVE / UP
t=258s INPUT_CONTROL_READY code=0
```

旧逻辑把物理 A/B 按下转换成一次触摸点击；触摸事件使 pod 改用触屏输入，
之后手柄帧仍在发送但游戏不再响应。

## 本版改动

1. 删除“物理面键 → 触摸点击”整条路径及配套的重试状态。
2. 自动进入：有手柄通道时先发虚拟手柄 A；仅当完全没有手柄通道时才用指针。
3. Keybinds 回包从 12 个字段收窄为 4 个：
   `_overrideControllerMapKeyList`、`_overrideControllerMapValueList`、
   `enableInputHotSwap`、`disableFallbackControllerType`。
   `inputData`（含 scriptVersion 与灵敏度）、`rewired*`、`vibration*` 不再回传。
4. `USD_*` 仍原样保留，承载 `isUsingJoypad` / `checkedDefaultUsingJoypad` /
   `joypadGUID`。

## 真实存档验收

用户 Switch 上的真实配置（164,081 字节）：

```
original_bytes=164081  reply_bytes=148918
reply_envelope = GENERAL_DATA + USD_0 + USD_122044085
USD_0          isUsingJoypad=false checkedDefault=false guid=0   identical=1
USD_122044085  isUsingJoypad=true  checkedDefault=true  guid=36  identical=1
GENERAL_DATA   fields=4  maps=2  KeyboardMap=0  hotswap=false
```

## 主机回归

`genshin_native_config`、`mihoyo_sdk_large_config`、`mihoyo_ui_language`、
`mihoyo_stream_input_policy`、`mihoyo_launcher_input_policy` 全部通过。

## 最终产物

最终 NRO：30379729 字节。

SHA256：`043F2616B02E0D4B4359209E5ED9E710A64EFF6BCB22F5F080FF884849B39FFC`

- NRO0 / ASET / NACP 0x4000 校验通过；
- 16 个语言槽全部填充，标题 `yuanshi launcher`、作者 `Steve1451-dot`；
- 显示版本 `0.2.122`；
- 本次构建无新增编译警告（旧的 stb_image_write 第三方警告除外）。

## 仍缺的实机证据

- 标题页是否接受虚拟手柄 A。若不接受，需要保留一次指针点击，
  但必须在点击后立即恢复纯手柄帧。
- 进入游戏后手柄是否持续有效（本版应不再出现触摸注入）。
- 键位是否在下次进入时仍在。

安装包不含账号、运行日志、登录库、SDK 用户配置、密钥、二维码或测试标记。
