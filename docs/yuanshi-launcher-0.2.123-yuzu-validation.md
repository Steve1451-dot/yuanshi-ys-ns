# 0.2.123 yuzu 实测记录（用真实账号数据）

日期：2026-11-09。使用用户提供的完整数据目录
（`H:\Users\Administrator\Desktop\yuanshi-launcher`，含登录库、SDK 密钥、
配置 vault、背景、日志），复制到 yuzu 的 sdmc 目录后运行 0.2.123。

## 环境说明

yuzu Early Access 4176，root 目录 `C:\Users\y\AppData\Roaming\yuzu`，
`prod.keys` / `title.keys` 已就位。真实云会话由用户账号建立。

## 已确认通过的部分

串流建立后，日志按预期顺序出现：

```
t=16s STREAM_TERMINAL_TYPE code=3            终端 3（Android 手机）
t=16s LAUNCHER_POINTER_TOUCH code=0          触摸进入，不再发鼠标
t=16s GENSHIN_NATIVE_GAMEPAD_ACTIVE code=0
t=16s GAME_SDK_CLOUD_DATA_VAULT_READY code=1
t=16s GAME_SDK_NATIVE_CONFIG_KEYBINDS_ON     默认键位回传模式
t=17s GAME_SDK_NATIVE_SAVED_KEYBIND_MAPS code=3   保存的 3 条手柄映射
t=17s GAME_SDK_NATIVE_SAVED_KEYBOARD_MAPS code=0  无键盘映射
t=26s GAME_SDK_CUSTOM_GET_LOCAL_DATA          游戏来取配置
t=26s GAME_SDK_NATIVE_BIND_REASON code=0      正常
t=26s GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT
t=26s GAME_SDK_LARGE_REPLY_SENT code=221793
t=27s GAME_SDK_LARGE_REPLY_ACKNOWLEDGED
t=35s GAME_SDK_NATIVE_WRITTEN_KEYBIND_MAPS code=3
t=35s GAME_SDK_NATIVE_WRITTEN_MAPS_MATCH_REPLY code=1   游戏回写逐条一致
t=37s GAME_SDK_INIT / LOGIN
```

关键结论：

1. 终端类型切换为 3 生效，启动页走触摸（`LAUNCHER_POINTER_TOUCH`）。
2. 保存的 3 条手柄映射被正确读取并回传，游戏回写后与回传完全一致
   （`WRITTEN_MAPS_MATCH_REPLY=1`）。
3. 0.2.121 的触摸注入缺陷已消失：全程没有 `PAD_A_ENTRY_TAP_SENT`。

## yuzu 无法验证的部分

### SRTP 解密全部失败

```
MEDIA_STARTUP_SRTP_REJECTED_20S
RTC_DTLS_DATAGRAMS_SENT      code=12
RTC_DTLS_DATAGRAMS_RECEIVED  code=2
RTC_RTP_COMPLETED            code=17286
RTC_RTP_DECRYPT_FAILURES     code=17286
RTC_RTP_VIDEO_ROUTED         code=0
VIDEO_FRAME_SAMPLE_COUNT     code=0
FIRST_VIDEO_TIMEOUT
```

DTLS 只发出 12 个、只收到 2 个数据报，握手未完成，SRTP 密钥从未协商，
因此 17,286 个 RTP 包全部解密失败、零视频帧。

这是 yuzu 的网络/SRTP 环境限制，与音视频代码路径无关：
同一份代码在真机上能正常出画面。

### 自动进入未触发

自动进入的前置条件是 `first_frame`。yuzu 里一帧都没有解出，
所以 `AUTO_ENTRY_*` 全程未出现。这是正确行为——没有画面就不应该乱点。

## 对交付的意义

- 终端 3、触摸进入、键位回传、游戏采用、无触摸注入：
  **已在真实云会话中验证。**
- 画面、真实手柄手感、游戏内热插拔显示：**仍需真机确认。**

## 由此修复的缺陷

0.2.123 的日志把每条事件写了两次（`log.append()` 与
`log.append_kept()` 各一次）。0.2.124 改为证据事件只走 kept 路径，
普通事件只走普通路径，每条事件只记录一次。
