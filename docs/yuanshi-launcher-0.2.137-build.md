# 0.2.137 本地构建与验证

日期：2026-11-12（Asia/Singapore）。基于当前 0.2.136 工作区及本轮标题页输入修正。
未提交、推送或创建 GitHub Release；未部署到用户 Switch。

## 离线回归

13 个主机目标通过：`idle_gamepad_policy`、`mihoyo_launcher_input_policy`、
`mihoyo_stream_input_policy`、`mihoyo_rtc_session`、`mihoyo_ui_language`、
`genshin_native_config`、`mihoyo_sdk_large_config`、`mihoyo_sdk_cloud_data_store`、
`mihoyo_input_packet`、`mihoyo_motion_input`、`gyro_stick_assist`、
`controller_system_buttons`、`input_transport_policy`。

真实 Session + 合成传输测试验证：ys/其他应用、Android/网页终端的范围隔离；
300 次空闲更新仅保留首次报告；触摸 Down/Up 中间无空闲帧插队；真实长按仍持续发送；
释放失败仍重试；强制 UI 中性帧、设备重新声明、输入清理及通道恢复均产生新报告。
游戏入口信号之后强制原来的报告刷新节奏，另有 10 次连续报告的回归断言。

`tests/cloudgenshin_monitor_test.ps1` 四组监控测试通过，新增三项固定安全事件能保留，
不把未知事件或任意内容写入报告。离线的 fake transport error 是预期故障注入。

本轮修改文件的 `git diff --check` 通过；全工作区仍有此前
`tests/peer_dtls_setup_test.c` 的空白差异告警，未为清理告警去改动无关媒体代码。

## Switch 产物

- `build-switch.ps1`：devkitA64 Release 编译、链接、NRO 打包完成。
- 构建过程中最后修改的流视图与日志入口文件，已通过第二次增量构建重新编译后链接。
- NRO0 / ASET / NACP 元数据检查通过，16 个语言槽、版本、作者与图标均匹配。
- 第三方 stb_image_write 的既有缺省初始化警告仍存在。
- 最终 NRO：30,396,113 字节。
- 最终候选 SHA256：`D60FDEF11D1F43B116E5C9D65C8D145EA4039A20A46757317ACF706EDC5CEDB8`。
- 原 0.2.136 已保留在 `dist/yuanshi-launcher-0.2.136.nro`，校验与原记录一致。

## yuzu 真实云会话

使用已有专用测试环境与已授权的登录会话，通过后台脚本运行，不操作窗口、不读取或输出登录库、
令牌、Cookie、二维码；首轮上限 300 秒，原生 A 对照轮上限 240 秒。
不能以本机发送成功、SDK 回写一致或零解码错误代替实际游戏操作验收。

### 首轮：只去除输入互扰，不改变触摸进入方式

`reports/report-0137-idle-input/status.json` 对应首次内部构建，SHA256
`B68E635FEC3FE61F3EE921553A862834BDD112DEF34BBC3B11464AE051B1B590`，
不是最终对照代码的产物。结果为 `HarnessDeadlineReached`，结束快照 294 秒；
8,308 帧解码、0 解码错误、48 次已完成触摸尝试，游戏进入信号仍为 false。
实际查看帧文件，确实停在可见的“点击进入”标题页。
去重启用与旧周期 reset 被忽略均有日志，但这轮没有证明进入问题已解决。

### 第二轮：受 `.pad` 标记限制的原生 A 对照

对应最终候选构建，`reports/report-0137-pad-fallback/status.json`。
结果为 `HarnessDeadlineReached`，结束快照 236 秒；6,549 帧解码、0 解码错误、
0 音频输出错误，已观察到 `AUTOTEST_ENTRY_PAD_FALLBACK_PRESS` 与释放完成事件，
游戏进入信号仍为 false。再次查看实际帧文件，仍是“点击进入”标题页。

两轮都没有解决本次标题页故障，不能称为游戏可操作验证通过。
修复输入互扰、去掉周期 reset，以及穿插当前 Xbox 身份的 A 都不足以解释／修复
这两次会话。下一步应追控制连接的实际接收与处理，而不是继续增加点击次数、
强制修改账号的原生键位、关闭校验或假设缺少某条回执就代表失败。

本轮脚本没有读取登录库、发送截图到外部、调用窗口操作或要求用户扫码。
两轮均由原有 SDK 停止与 harness 清理路径结束，运行结束后已清除本轮创建的
临时停止标记；未删除其他测试标记或账号文件。

### 监控缺口修正

另发现原生键位链的五个固定事件只写进了历史证据白名单，没有写进最前面的解析器
指标白名单，导致先被丢弃、后面白名单也无从恢复。已同时补齐两层并增加回归。
这影响“报告有没有记录到证据”，不能倒推游戏没有收到配置。
补齐解析器后，在第二轮现存安全日志中重新核对得到：保存 3 条手柄映射、
已发回传、写回 3 条、`WRITTEN_MAPS_MATCH_REPLY=1`。这是配置数据往返一致的
证据，不是标题页或自定义按键实际响应的证据。第二轮运行时的 harness 已经加载旧解析器，
其原始 `status.json` 未被事后改写；新增字段从后续运行开始自动记录。

## 未覆盖

尚未在 Switch 实机验收本版；原生键位跨重启、手感、热插拔与长时间稳定性仍待确认。
本轮没有修改或验收先前的明文媒体兼容回退，不能据此宣称媒体安全策略已经完成专项审查。
安装包不得包含测试标记、账号、密钥、游戏设置库或运行日志。
