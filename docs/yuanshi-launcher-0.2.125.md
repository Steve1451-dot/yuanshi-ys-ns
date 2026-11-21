# 圆石启动器 0.2.125 alpha：日志可靠落盘与真实数据复验

## 本版修复

### 0.2.124：证据事件重复记录

`cloudgenshin_tab.cpp` 的事件入口同时调用了 `log.append()` 和
`log.append_kept()`，导致每条证据事件写两遍。现在只有一个分支：

```cpp
if (IsEvidenceEvent(name)) log.append_kept(name,code,http);
else log.append(name,code,http);
```

### 0.2.125：关键事件立即落盘

`GAME_SDK_*`、`INPUT_SOURCE_*`、`GAMEPAD_SERVER_MODE_NOT_REPORTED_20S`、
`MEDIA_STARTUP_SRTP_REJECTED_20S` 原本不在触发保存的名单里，只留在内存，
靠周期性保存才写入。真机掉线或闪退时，恰好会丢掉判断键位是否被采用的证据。
现在这些事件会立即排队写盘。

### 更正：yuzu 日志目录

此前“yuzu 完全不写日志”的判断是查错了目录。yuzu 当前配置的 sdmc 是：

```
C:\Users\y\Documents\Codex\CloudGenshin-Debug\runtime\sdmc
```

不是 `%APPDATA%\yuzu\sdmc`。0.2.125 在真实数据会话中已经实时写出
92 KB 日志，串流过程中即可读取，不需要等退出。

## 0.2.125 真实数据复验（yuzu）

使用用户的真实数据目录（登录库、SDK 密钥、配置 vault、3 条手柄映射），
复制到 yuzu 当前 sdmc 后运行 0.2.125。关键日志：

```
t=26s STREAM_TERMINAL_TYPE code=3            终端 3（Android 手机）
t=26s LAUNCHER_POINTER_TOUCH code=0          触摸进入，不发鼠标
t=26s GAME_SDK_NATIVE_SAVED_KEYBIND_MAPS code=3
t=38s GAME_SDK_CUSTOM_GET_LOCAL_DATA        游戏来取配置
t=38s GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT
t=38s GAME_SDK_LARGE_REPLY_SENT code=221793
t=38s GAME_SDK_LARGE_REPLY_ACKNOWLEDGED
t=46s GAME_SDK_NATIVE_WRITTEN_KEYBIND_MAPS code=3
t=46s GAME_SDK_NATIVE_WRITTEN_MAPS_MATCH_REPLY code=1
```

结论：

1. 3 条手柄映射被读取并回传，游戏回写后逐条一致。
2. 终端 3 生效，启动页走触摸。
3. 全程没有 `PAD_A_ENTRY_TAP_SENT`，0.2.121 的触摸注入缺陷未复现。
4. 日志无相邻重复行，且在串流过程中实时落盘。

第二次连接（手动重连）同样完成键位回传与采用，说明流程可重复。

## yuzu 仍无法验证画面

yuzu 中两次连接都在 SRTP 解密阶段失败：

```
RTC_RTP_COMPLETED            19198
RTC_RTP_DECRYPT_FAILURES     19198
RTC_RTP_LAST_DECRYPT_ERROR   7
RTC_RTP_VIDEO_ROUTED         0
RTC_DTLS_DATAGRAMS_SENT      12
RTC_DTLS_DATAGRAMS_RECEIVED  2
```

DTLS 握手未完成，SRTP 密钥未协商，所以零视频帧。这是 yuzu 的
网络/SRTP 环境限制，不是本次改动的代码路径。

同一份代码在真机上的 0.2.121 日志（`mtp-pull`）显示画面完全正常：

```
RTC_RTP_VIDEO_ROUTED      5736
VIDEO_DECODED_FRAMES      1437
VIDEO_HARDWARE_FRAMES     1437
RTC_RTP_DECRYPT_FAILURES  5
```

因此画面、真实手柄手感、游戏内触屏/手柄热插拔仍需真机确认。
yuzu 能验证的是：终端选择、触摸进入、键位回传、游戏采用、日志可靠性。

## 产物

- 文件：`dist/yuanshi-launcher-0.2.125.nro`
- 大小：30,379,729 字节
- SHA256：`22247F471E9447524AFA398CA1D96DF74C6D75638B503EAB1665B5E0F53DAC80`
- 显示版本：`0.2.125`
- 标题：`yuanshi launcher`，作者：`Steve1451-dot`

## 仍待真机确认

- 进入游戏后手柄是否持续有效（无 0.2.121 的输入中断）。
- 游戏内「控制设备」是否显示为手柄，触屏与手柄能否自动切换。
- 退出再进入后，自定义键位是否仍在。
