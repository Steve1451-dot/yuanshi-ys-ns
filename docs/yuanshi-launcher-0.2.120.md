# 圆石启动器 0.2.120 alpha：先修证据，再谈键位恢复

## 重要更正：0.2.118 第二次实机其实请求了配置

0.2.118 的结论“run2 整场没有 `cloud_get_data`”是**错的**，来源是读错了日志文件。

`cloudgenshin-safe.log` 是字节有上限的快照，第 131 行写着
`omitted_event_count=3352`，被丢掉的是 t≈5s 到 105s 整段。而同一会话的增量归档
`stream-logs/stream-safe-01.log`（5530 行，没有丢失）显示：

```
489  t=14s GAME_SDK_CUSTOM_REQUEST
490  t=14s GAME_SDK_REPLY_SENT
492  t=15s GAME_SDK_ACK_RECEIVED
493  t=15s GAME_SDK_CLOUD_DATA_RESTORE_SKIPPED
822  t=22s GAME_SDK_CLOUD_DATA_STORED
825  t=24s GAME_SDK_INIT
826  t=24s GAME_SDK_LOGIN
1656 t=47s GAME_SDK_WILL_ENTER_GAME
```

游戏确实在 t=14s 读取了保存配置，是 Off 模式在 t=15s 主动跳过。
所以“游戏不会来取配置”不成立，键位恢复路径每次都有机会执行。

这个更正不改变“0.2.118 手柄正常”的事实，但推翻了当时对原因的判断。

## 本版改动

### 0. 键位恢复默认关闭，但已就绪

键位恢复逻辑本身已经完整可用，只是默认不开启，因为唯一实机验证过的安全状态是
“只保存、不回传”。要启用：

1. 在 `switch/yuanshi-launcher/` 创建
   `cloudgenshin-native-config.txt`，内容为一行 `CGNX_NATIVE_KEYBINDS_RESTORE_V1`；
2. 进入一次 ys，退出；
3. 再进入 ys，检查键位是否还在、是否仍是手柄模式。

一旦出现键鼠界面，删除该文件即可立刻回到已验证安全的默认状态。

### 1. 证据事件不再被日志丢弃（最高优先级）

- `SafeEventLog` 新增 `append_kept()`：SDK／云端协议里程碑不参与 2048 行环形淘汰，
  也不被 128 KiB 快照预算裁掉，并保持按行号时间顺序输出。
- `cloudgenshin_tab.cpp` 把所有 `GAME_SDK_*`、登录、`STREAM_ENTRY`、
  `RTC_PEER_STATE`、`QUEUED_POSITION` 归为证据事件。
- 单测覆盖：2600 条普通计数 + 证据事件仍然保留；增量归档在环形淘汰后仍能补发。

### 2. 自定义接口按函数分别记录

旧代码只有一个 `GAME_SDK_CUSTOM_REQUEST` 标记位，读 `account_id`、读配置、写配置
全部塌缩成一行，这正是日志误读的根源。现在分为：

- `GAME_SDK_CUSTOM_GET_ACCOUNT_ID`
- `GAME_SDK_CUSTOM_GET_LOCAL_DATA`
- `GAME_SDK_CUSTOM_SET_LOCAL_DATA`
- `GAME_SDK_CUSTOM_OTHER`

### 3. 键位负载改为 M0 最小集

`PrepareNativeControllerRestore()` 在 Keybinds 模式下现在只返回：

```json
{"GENERAL_DATA": "{\"_overrideControllerMapKeyList\":[...],\"_overrideControllerMapValueList\":[...]}"}
```

- 不包含 `enableInputHotSwap`、`rewiredEnableKeyboard`、`rewiredDisableKeyboard`；
- 不回显 `inputData`、画质、语言或任何无关字段；
- **不再伪造任何输入模式开关**——上次的推测值没有实机依据。

### 4. 映射筛选改为“只排除键鼠触屏”

旧实现是黑名单（值含 `KeyboardMap` 就删），`MouseMap`、自定义控制器映射会漏过。
现在：

- 只排除 `KeyboardMap` / `MouseMap` / `TouchMap`，或键名含 keyboard／mouse／touch；
- 其余映射只要值看起来是序列化映射（含 `<...>` 或 `{...}`）就保留，
  即使根元素名不在已知列表里（真实存档可能有自定义控制器、吉他、街机摇杆）；
- 完全不像映射的值丢弃并计数；
- 空字符串条目逐条丢弃并计数，不再导致整体失败；
- 上限在筛选之后判断；超过 64 条时整次放弃，避免发出残缺键位布局；
- 新增原因码 `GAME_SDK_NATIVE_BIND_REASON`，区分解码失败、缺字段、
  长度不一致、超限、无手柄映射、超预算、编码失败。

> 这个选择是刻意的：目标是“映射必须还在”。宁可保留一个未知根元素的映射并计数，
> 也不因为白名单不全而静默恢复成空。

### 5. 新增配置结构报告（无需连电脑）

每次会话若存在已保存配置，会生成
`game-sdk-hk4e-structure.txt`，内容**只有**：

- 外层与 `GENERAL_DATA` 的字段名、JSON 类型；
- 布尔字段的值、数组长度、字符串字节数；
- 每个映射条目的键长度、值长度、根元素名（如 `CJoystickMap`）、
  是否被判为键盘、是否被判为手柄。

不包含任何映射内容、账号或凭据。这样即使不连电脑、不解密 vault，
也能确认真实格式是否被正确识别，并据此扩展判定规则。

### 5. 默认改回 Off

唯一在实机验证过的安全状态是“只保存、不回传”。本版默认恢复为 Off：

- 文件不存在 → Off
- 空文件或 `CGNX_NATIVE_CONFIG_RESTORE_OFF_V1` → Off
- `CGNX_NATIVE_KEYBINDS_RESTORE_V1` → 键位 M0（需手动创建）
- `CGNX_NATIVE_CONFIG_RESTORE_V1` → 完整回传（仅复现旧问题）
- 未知内容（含 BOM、尾随空格）→ Off，并单独记录
  `GAME_SDK_NATIVE_CONFIG_MARKER_UNKNOWN`

### 6. 防止键位被默认值覆盖

每次会话启动时，若存在已保存配置，另存一份明文快照
`game-sdk-hk4e-session-start.json`。它是新增文件，不改动、不删除任何加密原文件，
用于在游戏把默认配置写回后仍能找回用户上一次的键位。

### 7. 采用证据

`cloud_set_data` 之后会比较游戏回写的映射与本次回传的映射（只比较控制器映射，
不记录内容），输出 `GAME_SDK_NATIVE_WRITTEN_MAPS_MATCH_REPLY`。
这才是“云端采用”的证据；本机 ACK 不算。

## 尚未验证

- M0 负载在真实 `GENERAL_DATA` 上是否被接受，仍未实机确认。
- 真实存档的字段名、映射根元素格式仍未解密核对。
- 完整回传 A/B（`CGNX_NATIVE_CONFIG_RESTORE_V1`）未复测。

## 验证方法

1. 安装本版，不创建开关文件，确认日志出现 `GAME_SDK_NATIVE_CONFIG_RESTORE_OFF`。
2. 进入 ys，确认手柄正常，并确认日志出现：
   - `GAME_SDK_CUSTOM_GET_LOCAL_DATA`
   - `GAME_SDK_CLOUD_DATA_VAULT_READY` 或 `..._EMPTY`
   - `GAME_SDK_SESSION_BASELINE_SAVED`
3. 从 `stream-logs/` 取增量归档，不要只看 `cloudgenshin-safe.log`。
4. 若要试键位 M0：创建内容为 `CGNX_NATIVE_KEYBINDS_RESTORE_V1` 的
   `cloudgenshin-native-config.txt`，重进，检查：
   - `GAME_SDK_NATIVE_CONFIG_KEYBINDS_ON`
   - `GAME_SDK_NATIVE_BIND_REASON`（0 才是正常）
   - `GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT`
   - `GAME_SDK_NATIVE_UNKNOWN_MAP_COUNT`（未知根元素数量，仅诊断）
   - 进入游戏后自定义键位仍在且仍是手柄模式
   - 游戏回写后 `GAME_SDK_NATIVE_WRITTEN_MAPS_MATCH_REPLY=1`

5. 无论是否开键位，检查 `game-sdk-hk4e-structure.txt`：
   - 里面应列出真实 `GENERAL_DATA` 字段名；
   - `entry=` 行会显示每个映射的 `root=`（真实根元素名）与
     `keyboard=` / `controller=` 判定；
   - 如果某个真实手柄映射被标成 `keyboard=1`，说明判定过宽，需要收窄；
   - 如果全是 `controller=0` 但仍被保留，说明走的是“形似映射”路径。

任一组出现键盘模式，立即删除开关文件回到 Off，不要发布。
