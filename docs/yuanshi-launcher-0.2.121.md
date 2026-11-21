# 圆石启动器 0.2.121 alpha：回传手柄键位与输入模式设置

## 这次找到的真正原因

前几版一直没修好，是因为我拿错了判断依据。你第一次给我的存档里
`_overrideControllerMapKeyList` 是空的，我据此得出“键位不在配置里”的结论——
那是**你还没在游戏里改过键位**时的状态。

你在游戏里改过键位后，真实数据立刻出现了：

```
GENERAL_DATA  8,502 字节  →  65,579 字节
_overrideControllerMapKeyList   = 2 条
_overrideControllerMapValueList = 2 条
  [0] 55,920 字节  <JoystickMap hardwareName="XInput Gamepad">  137 条按键映射
  [1]    978 字节  <JoystickMap>                              1 条映射
KeyboardMap 0 条 / MouseMap 0 条
```

## 热插拔开关在哪里

你在游戏里打开的这两个设置：

```
控制设备                = 手柄
主界面控制设备自动切换  = 打开
```

它们不在 `GENERAL_DATA`，而是在 `USD_<uid>` 里：

```
USD_0          isUsingJoypad=false  checkedDefaultUsingJoypad=false  joypadGUID=""
USD_122044085  isUsingJoypad=true   checkedDefaultUsingJoypad=true   joypadGUID="d74a350e-..."
```

`checkedDefaultUsingJoypad` 从 `false` 变成 `true`，`joypadGUID` 被写入 Xbox 手柄的
GUID——这正是你打开“自动切换”之后游戏写下的状态。

## 为什么之前一直失效

日志写得很直接：

```
t=5s   GAME_SDK_NATIVE_CONFIG_RESTORE_OFF
t=15s  GAME_SDK_CUSTOM_GET_LOCAL_DATA      ← 游戏来取配置
t=15s  GAME_SDK_CLOUD_DATA_RESTORE_SKIPPED ← 被 Off 模式跳过
```

游戏每次都会来取，是启动器主动不回传。而 0.2.120 的 M0 负载又只回传两个映射数组，
把输入模式字段和 `USD_*` 都排除在外，所以即使回传，游戏也拿不到热插拔状态。

## 本版改动

### 1. 默认改为 Keybinds

没有开关文件时，默认回传已保存的手柄键位和输入模式设置。

### 2. 回包内容

`GENERAL_DATA` 包含：

- `_overrideControllerMapKeyList` / `_overrideControllerMapValueList`：只保留手柄映射，
  排除键盘／鼠标／触屏；未知根元素但形似映射的也保留并计数；
- `enableInputHotSwap`、`inputData`、`rewiredEnableKeyboard`、`rewiredDisableKeyboard`、
  `rewiredEnableEDS`、`disableRewiredDelayInit`、`disableRewiredInitProtection`、
  `disableSetJoyInfoForWebViewOnPCMobile`、`disableFallbackControllerType`、
  `usingNewVibrationSetting`、`vibrationLevel`、`vibrationIntensity`：
  **全部按游戏保存的原值透传，不强制改写**。

`USD_*` 原样保留，不做任何修改。

### 3. 用真实存档验证过

用你 Switch 上那份 164,081 字节的真实配置跑现有代码：

```
keybinds maps=2 reason=0 reply=151,391 字节
reply_envelope = GENERAL_DATA + USD_0 + USD_122044085
USD_122044085  isUsingJoypad=true  checkedDefault=true  guid=36 字节  与原值完全一致
GENERAL_DATA   14 字段  2 条手柄映射  KeyboardMap=0
```

## 仍然存在的风险

- `USD_*` 是完整本地数据（任务、红点、背包等）。回传它意味着旧进度会覆盖云端已有进度。
  本版用“会话启动快照”做保险，但不改变这个语义。**如果你在两个设备上交替玩，需要注意。**
- `enableInputHotSwap` 按原值透传（你的是 `false`）。如果你在游戏里打开“自动切换”，
  存档里对应字段会变成 `true`，本版会跟着透传 `true`。
- 云端是否真的采用这份配置，需要实机确认。

## 验证方法

1. 安装本版，不需要创建任何开关文件。
2. 进 ys，确认日志出现 `GAME_SDK_NATIVE_CONFIG_KEYBINDS_ON`、
   `GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT`。
3. 进游戏后确认：手柄直接用、键位是你改过的、不必再手动切“控制设备”。
4. 退出重进，再看一次。
5. 若要只保存不回传，创建空文件 `cloudgenshin-native-config.txt`。
