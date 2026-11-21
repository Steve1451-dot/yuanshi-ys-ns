# 圆石启动器 0.2.122 alpha：进入游戏不再注入指针事件

## 0.2.121 实机结果：配置回传成功，输入被打断

用户装上 0.2.121 后测试，日志分成两个独立问题。

### 一、键位与输入模式回传确实成功了

```
t=194s GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT
t=194s GAME_SDK_LARGE_REPLY_SENT code=223009
t=195s GAME_SDK_LARGE_REPLY_ACKNOWLEDGED
t=203s GAME_SDK_NATIVE_WRITTEN_KEYBIND_MAPS code=2
t=203s GAME_SDK_NATIVE_WRITTEN_MAPS_MATCH_REPLY code=1
```

`WRITTEN_MAPS_MATCH_REPLY=1` 表示游戏自己回写的映射与本次回传**完全一致**。
这是目前最强的“云端已采用”证据，比本机 ACK 可靠。

### 二、输入被触摸事件打断（0.2.121 的缺陷）

```
t=218s INPUT_LOCAL_PRESS_SAMPLED       用户按下手柄
t=218s PAD_A_ENTRY_TAP_SENT            代码把它变成了一次触摸点击
t=222s RESET_GAME_INPUT_CONTROL_SENT code=4
t=222s INPUT_SOURCE_DECLARED code=4
t=241s TOUCH_SENT_DOWN / MOVE / UP
t=258s INPUT_CONTROL_READY code=0
```

旧逻辑有一支“物理 A/B 按下 → 发送一次触摸点击”的标题页快捷路径。
这次它在用户正常操作手柄时被触发，触摸事件让 pod 切到触屏输入，
之后手柄帧虽然仍在发送，游戏已不再响应。

用户的判断从 0.2.119 起就是对的：**不能发指针**。

## 本版改动

1. **删除“面键 → 触摸点击”快捷路径**。按 A/B 只产生手柄帧，
   不再转换成触摸点击，也不再补发触摸释放。
2. **自动进入改为手柄优先**。有手柄通道时先发虚拟手柄 A；
   只有完全没有手柄通道时才使用指针点击。
   日志会显示 `AUTO_ENTRY_PAD_PRESS` 而不是 `PAD_A_ENTRY_TAP_SENT`。
3. **回包再收窄**。0.2.121 把 `inputData`、`rewired*`、`vibration*`
   等 12 个字段一起回传；其中 `inputData` 含 `scriptVersion` 与全套灵敏度，
   属于本地设备描述。现在只回传 4 个字段：
   - `_overrideControllerMapKeyList`
   - `_overrideControllerMapValueList`
   - `enableInputHotSwap`
   - `disableFallbackControllerType`

   `USD_*` 仍然原样保留，因为它承载
   `isUsingJoypad` / `checkedDefaultUsingJoypad` / `joypadGUID`。

## 真实存档验收

用用户 Switch 上的真实配置（164,081 字节）：

```
original_bytes=164081  reply_bytes=148918
reply_envelope = GENERAL_DATA + USD_0 + USD_122044085
USD_0          isUsingJoypad=false checkedDefault=false guid=0     identical=1
USD_122044085  isUsingJoypad=true  checkedDefault=true  guid=36    identical=1
GENERAL_DATA   fields=4  maps=2  KeyboardMap=0
```

## 主机回归

相关目标全部通过：`genshin_native_config`、`mihoyo_sdk_large_config`、
`mihoyo_ui_language`、`mihoyo_stream_input_policy`、`mihoyo_launcher_input_policy`。

## 仍需实机确认

- 手柄能否在标题页直接进入游戏。若仍不能，说明 pod 不接受虚拟手柄 A，
  需要保留一次指针点击，但要在点击后立刻恢复手柄帧。
- 进入游戏后手柄是否持续有效（本版应不再有触摸注入）。
- 键位是否在下次进入时仍在。

## 最终产物

最终 NRO：见下方补记。

安装包不含账号、运行日志、登录库、SDK 用户配置、密钥、二维码或测试标记。
