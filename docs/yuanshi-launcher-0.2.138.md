# 0.2.138 原生配置与输入协议对照

日期：2026-11-13（Asia/Singapore）。这是排障候选版，不是已验收的完整可玩版。

## 原生键位存取

APK 7.1.0 的 `BaseSdkHolder.interceptGameData`：`cloud_set_data` 把字符串按 UTF-8
写入对应文件；`cloud_get_data` 的回调 `BaseSdkHolder.m` 直接读出字符串回包，
不解析或改写 `GENERAL_DATA`。此前 Keybinds 路线会删去一部分游戏字段，不能称作
完整复刻这条存储契约。

新增独立的 Exact 对照：在应用目录的 `cloudgenshin-native-config.txt` 写入
`CGNX_NATIVE_CONFIG_EXACT_V1`。它保留原始字符串、输入设备配置、全部映射与未知字段，
不会像旧 Full 实验那样强制 `enableInputHotSwap=true`。仅限 ys，且仍有结构和大小校验。
普通用户缺省仍为 Keybinds，未将未验收的 Exact 强行推广到所有安装。

`GAME_SDK_NATIVE_EXACT_CONFIG_PREPARED` 只表示回包准备完成，不表示已送达、
键位已应用或手柄已经能玩。游戏回写的映射相同也只证明数据往返。

增加真实加密存储与 SDK 队列的三次独立启动回归：首次保存、第二次恢复并修改、
第三次恢复修改结果。这是离线存储验证，不替代实际游戏操作与 Switch 重启验收。

## 输入协议补齐

APK 的 `ClientCoreImpl::onGamepadPressed` 与 `sendTouchEventList` 会写入
`RtcDataChannel.message_trace_id`（field 18，uint64）。触摸、手柄使用独立计数器；
`libnetio` 的序列化器在 0x21fcf4 起写入 tag `90 01`。此前客户端漏掉该原生字段。

本版仅在 ys Android 终端补入这两个输入计数器，不修改 Xbox 按钮位、摇杆、触摸坐标、
认证、媒体或其他游戏。序号不是输入回执，不能仅据此宣称标题页问题解决。

## 对照证据

早间使用 0.2.137、关闭旧配置回传：`reports/report-0137-no-restore-morning/status.json`。
云会话成功建立，结束快照 294 秒，解码 8,237 帧，0 解码错误，游戏进入信号为 false；
目视确认仍在“点击进入”。因此旧配置回传不是这次标题页输入故障的唯一解释。
测试开始前保留的 4 份加密游戏设置快照已恢复并逐份校验；登录库没有读取或改写。

新版构建与后续真实会话结果见对应构建记录，未验证项目不得写成通过。

第一轮 0.2.138（Exact + 原生输入编号）实际进入了游戏场景，SDK 在安全日志 t=66s
报告 `WILL_ENTER_GAME`，结束快照解码 7,881 帧。但右摇杆探针与两次 Plus 探针
未出现预期相机/菜单响应，仍不能称作可玩。该内部产物 SHA256 为
`1A1261398C06610B93698807EAA97537EE6C9DFC146F4A220B864334163F9B6E`。

随后补充仅诊断模式启用的手柄变化边沿追踪（与触摸共用最多 32 条额度，不记录
逐帧中性包或长按刷新），记录经过本地过滤后的最终按钮/摇杆数值。原计划再用 Keybinds
对照；用户随后要求先离线修改，故没有启动这轮云会话。

最终候选版只完成离线回归与 Switch 构建，未再次连接云端、部署实机或发布 Release。
其 SHA256 为 `D6C2FC3E10B9C500B5CC51B4EBC20701C09EFD901692C8B05CB89D703858C159`，
与上面的首次内部实验产物不同。原始加密键位基线已恢复校验，临时对照标记已撤掉；
只清理本轮创建的 4 份重复加密备份，用户原文件完整保留。
