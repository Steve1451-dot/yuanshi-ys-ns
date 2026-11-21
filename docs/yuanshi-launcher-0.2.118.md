# 圆石启动器 0.2.118 alpha：ys 配置只保存、默认不回传

## 已确认与仍不确定的部分

2026-11-09 用户安装 0.2.117 并首次进入原神，仍然看到 E/Z/Q 键鼠提示，
实体手柄没有接管游戏。完整安全日志显示：

- 终端类型仍为 10，RTC 数据通道已打开，游戏配置中的 Xbox／PS 标志均为 0。
- 本地手柄帧发送成功、设备列表已宣告、输入来源已声明为手柄，但没有
  `SERVER_GAME_CONTROL_MODE`，20 秒后出现 `GAMEPAD_SERVER_MODE_NOT_REPORTED_20S`。
- 0.2.117 在 t=15s 读回并修改了已保存的原生键位配置，回包 218889 字节并收到 ACK，
  但 ACK 只证明本机可靠队列接收，不证明云端游戏采用。

0.2.63 基线没有恢复保存配置，终端 10 + Xbox 手柄可以控制右摇杆镜头和 Plus 菜单。
0.2.115／0.2.116／0.2.117 都恢复了保存配置，三版都停在键盘模式。
这组对比把“回传完整游戏配置”列为当前最强回归变量。2026-11-09 两次 0.2.118
实机进入已完成默认关闭路径的验证，结果见下方“实机验证”。

## 本版改动

- ys 默认**不向游戏回传**已保存的 `cloud_local_data_*` 配置；`cloud_get_data`
  对这些键返回空字符串，与没有缓存的基线行为一致。
- 仍然保存游戏通过 `cloud_set_data` 写入的数据。SD 卡上的加密配置、账号隔离、
  密钥和原有文件都不删除、不改写格式。
- 恢复路径改为显式实验开关：只有手动创建
  `switch/yuanshi-launcher/cloudgenshin-native-config.txt`，且内容为一行
  `CGNX_NATIVE_CONFIG_RESTORE_V1`（末尾换行可有可无）时，才回传保存配置并请求
  `enableInputHotSwap=true`。删除该文件即回到默认关闭。
- 不修改游戏原生键位表、控制器 ID、灵敏度或任何本地改键；不把手柄转换成键盘。
- 新增固定安全日志事件，便于确认本版实际走了哪条路径：
  - `GAME_SDK_NATIVE_CONFIG_RESTORE_OFF`：默认关闭回传。
  - `GAME_SDK_NATIVE_CONFIG_RESTORE_ON`：实验开关已开启。
  - `GAME_SDK_CLOUD_DATA_VAULT_READY`：SD 配置已读入内存作保存合并，但不回传。
  - `GAME_SDK_CLOUD_DATA_RESTORE_SKIPPED`：游戏请求的缓存键被有意跳过。
  - `GAME_SDK_NATIVE_CONFIG_SAVE_ONLY`：本次会话仍会保存游戏写入的数据。

## 实机验证（2026-11-09）

同一台 Switch、同一账号、同一 Xbox 手柄，安装 0.2.118 后进入原神两次：

1. 第一次进入时登录态失效，重新扫码登录；启动器日志为
   `GAME_SDK_CLOUD_DATA_EMPTY`，即当时没有匹配的已保存配置。
   终端类型仍为 10，`GAME_SDK_NATIVE_CONFIG_RESTORE_OFF` 出现；
   游戏在 t=30s 通过 `cloud_set_data` 写入配置，日志出现
   `GAME_SDK_NATIVE_CONFIG_SAVE_ONLY`、`GAME_SDK_CLOUD_DATA_STORED`、
   `GAME_SDK_CLOUD_DATA_PERSISTED`。用户实机反馈手柄正常。
2. 第二次进入使用已保存登录，日志在 t=4s 出现
   `GAME_SDK_CLOUD_DATA_VAULT_READY code=1`，说明 SD 卡上的游戏配置已读入内存；
   同时出现 `GAME_SDK_NATIVE_CONFIG_RESTORE_OFF`。本次没有
   `GAME_SDK_LARGE_REPLY_SENT`、`GAME_SDK_NATIVE_INPUT_HOTSWAP_REQUESTED`
   或 `GAME_SDK_CLOUD_DATA_RESTORED`。用户再次确认手柄正常。

结论：在“已有保存配置”的情况下，0.2.118 默认没有把配置回传给游戏，
实体手柄可以正常控制。第二次会话中游戏没有请求该缓存键，因此没有出现
`GAME_SDK_CLOUD_DATA_RESTORE_SKIPPED`；这不影响“未回传”的结论。

第一次日志在 t=62s 仍出现 `GAMEPAD_SERVER_MODE_NOT_REPORTED_20S`，但同一会话
手柄实际可用，说明该 20 秒诊断不是可靠的失败判据，后续版本应调整或移除。

## 验证方法

1. 安装 0.2.118，确认 `cloudgenshin-native-config.txt` 不存在。
2. 首次进入 ys，观察是否还出现 E/Z/Q，以及左摇杆、右摇杆镜头、Plus 菜单是否生效。
3. 返回后提供 `cloudgenshin-safe.log`（或完整 `stream-safe-*.log`）。重点看：
   - 是否出现 `GAME_SDK_NATIVE_CONFIG_RESTORE_OFF`；
   - 如果游戏请求该缓存键，是否出现 `GAME_SDK_CLOUD_DATA_RESTORE_SKIPPED`；
   - 是否没有 `GAME_SDK_LARGE_REPLY_SENT` 和 `GAME_SDK_NATIVE_INPUT_HOTSWAP_REQUESTED`；
   - 游戏写入后是否仍出现 `GAME_SDK_CLOUD_DATA_STORED` / `PERSISTED`。
4. 如需 A/B，再创建开关文件重复一次，对比同一终端、同一手柄下的表现。

日志只能证明本机请求与保存状态；E/Z/Q 与手柄控制以实机画面为准。
2026-11-09 用户实机反馈手柄正常，默认关闭回传路径通过；打开实验开关后的
A/B 行为仍未复测。

## 原生键位保存与恢复

1. 用户在游戏内修改原生手柄键位。
2. 游戏通过 SDK `cloud_set_data` 提交配置；启动器在 SD 卡专用目录加密、原子保存。
3. 0.2.118 默认不在下次启动时回传该配置，避免连带恢复上次的键盘输入模式。
4. 打开实验开关后才会回传完整配置；后续若要只恢复键位字段、不带输入模式，
   仍需先确认 `GENERAL_DATA` 中具体哪个字段控制启动输入设备。

配置格式和原生键位字段可对照开源
[Collapse GeneralData 定义](https://github.com/CollapseLauncher/Collapse/blob/main/CollapseLauncher/Classes/GameManagement/GameSettings/Genshin/RegistryClass/GeneralData.cs)。
该来源不证明云端游戏采用哪个输入模式。
