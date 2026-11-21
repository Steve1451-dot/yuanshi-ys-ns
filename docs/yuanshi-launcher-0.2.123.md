# 圆石启动器 0.2.123 alpha：改用 Android 移动终端（触屏与手柄并用）

## 为什么换终端

0.2.121 实机日志显示，终端 10（CTT_WEB_KEYBOARD，PC 网页）下：

- 启动页使用**鼠标**进入：`LAUNCHER_POINTER_MOUSE`
- 鼠标事件会让 pod 切到键鼠输入：`RESET_GAME_INPUT_CONTROL_SENT code=4`
- 之后手柄帧仍在发送，但游戏不再响应

用户提出的方向是直接模拟手机：触屏可用、手柄可用、支持游戏内热插拔。
这与终端族的行为一致，比继续在 PC 终端上补丁更根本。

## 终端族的差别

| 终端 | 家族 | 启动页输入 | cps |
| --- | --- | --- | --- |
| 3 | CTT_ANDROID | 触摸 | `android_mihoyo` |
| 10 | CTT_WEB_KEYBOARD | 鼠标 | `pc_mihoyo` |

`UsesTouchLauncher()` 对 2/3/6/7/9 返回真，即使用官方触摸流。

终端 3 的三项连带效果：

1. 启动页走 `send_touch`，不再发鼠标移动/点击；
2. `cps` 自动变成 `android_mihoyo`；
3. 游戏内输入设置为移动端形态，原本就同时暴露触屏与手柄。

## 本版改动

1. 默认终端从 10 改为 **3（Android）**。
   `cloudgenshin-terminal.txt` 仍可覆盖，用于 A/B。
2. 自动进入按终端族分流：
   - 触摸族（含终端 3）：发触摸点击，日志 `AUTO_ENTRY_TOUCH_PRESS`
   - 非触摸族：先发虚拟手柄 A，日志 `AUTO_ENTRY_PAD_PRESS`
3. 物理面键不再转成指针点击（0.2.122 已移除）。
4. 触摸与手柄帧**同时存在**：每帧先发手柄帧，再发待发送的触摸事件。
   两者不互斥，所以可以一边用手柄、一边点屏幕。
5. 键位回包维持 0.2.122 的 4 字段：两个映射数组 +
   `enableInputHotSwap` + `disableFallbackControllerType`；
   `USD_*` 原样保留（承载 `isUsingJoypad` / `checkedDefaultUsingJoypad` /
   `joypadGUID`）。

## 与热插拔的关系

游戏内「主界面控制设备自动切换」的状态存在 `USD_<uid>` 的
`checkedDefaultUsingJoypad` 与 `isUsingJoypad`。回传 `USD_*` 后，
这个开关会跟着恢复，游戏即可在触屏与手柄之间自动切换。

回包里的 `enableInputHotSwap` 取自游戏保存的原值，不强制改写。

## 仍待实机确认

- 终端 3 下启动页是否接受触摸（日志应为 `LAUNCHER_POINTER_TOUCH`、
  `AUTO_ENTRY_TOUCH_PRESS`）。
- 进入游戏后手柄是否持续有效。
- 游戏内「控制设备」是否显示为手柄，且在触屏/手柄间可自动切换。
- 键位是否在下次进入时仍在。

## 最终产物

最终 NRO：30379729 字节。

SHA256：`5FAF232B554228E21683E00C6B99F59E3C2B110AC9756B78E72DAC5314BC60BA`

- NRO0 / ASET / NACP 0x4000 校验通过；
- 16 个语言槽全部填充，标题 `yuanshi launcher`、作者 `Steve1451-dot`；
- 显示版本 `0.2.123`。

安装包不含账号、运行日志、登录库、SDK 用户配置、密钥、二维码或测试标记。
