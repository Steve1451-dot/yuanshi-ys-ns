# 0.2.144 离线验证与构建校验

日期：2026-11-15（Asia/Singapore）。入口基线候选，不是完整可玩验收。

## Switch 产物

- `OPENNOW_BUILD_JOBS=2`，通过 `build-switch.ps1` 实际完成 devkitA64 Release
  编译、链接与 NRO 打包（128 步构建序列）。
- 版本脚本及 `tests/yuanshi_nro_metadata_test.ps1` 通过：NRO0、ASET、NACP、
  `0.2.144`、选定图标、作者与 16 个语言槽一致；HOME Shortcut 元数据同步构建。
- 本轮没有修改 `extern/`；构建仍有既有 stb 第三方缺省初始化警告，没有编译错误。
- 不可变候选：`dist/yuanshi-launcher-0.2.144.nro`，30,400,209 字节。
- SHA256：`4F90CBC33C9094AE1671AEC07620F956ED4DD565A23966D9A161818A08EDAC12`。
- 从本次 build 复制后逐份核对 SHA256，build/dist 一致。

## 离线回归

15 个 host 目标通过：

`mihoyo_launcher_input_policy`、`mihoyo_stream_input_policy`、`mihoyo_input_packet`、
`controller_system_buttons`、`idle_gamepad_policy`、`input_transport_policy`、
`genshin_native_config`、`mihoyo_sdk_large_config`、`mihoyo_rtc_session`、
`mihoyo_ui_language`、`mihoyo_game_profile`、`mihoyo_autotest_pad_policy`、
`mihoyo_controller_source_policy`、`mihoyo_touch_forward`、`genshin_input_experiment`。

- 新增固定触摸 60 次序列、120 ms 保持、发送失败/松手重试、手动接管、SDK 进入后
  释放、延迟调度和通道重建测试。实际 Session/报文代码也用该策略生成并验证 Down/Up，
  只替换网络传输；没有访问真实账号或云端。
- 既有三次独立生命周期的加密配置保存/恢复回归通过；证明本地往返，不证明云端采用。
- `tests/genshin_entry_baseline_test.ps1` 通过：旧入口标记已不参与代码路径，ys 不再
  使用 `.click` 替换基线，自动移动端入口不混用 A/B、键盘或坐标轮换。
- `tests/cloudgenshin_monitor_test.ps1` 通过：新基线事件/947 坐标保留，队列总人数
  100 允许、101 停止等既有边界不回退，历史诊断解析保留。
- 本轮涉及的 tracked 文件 `git diff --check` 通过（按仓库 CRLF 行尾处理）。

## 未做的验证

严格遵守本轮“不进游戏”的要求：没有启动 yuzu、登录、派发、排队或实机测试；
没有读取凭据、改写运行目录/游戏配置、部署 Switch、提交、推送或发布 Release。
本机 yuzu 没有 terminal override 文件，缺省仍为终端 3。原有实验标记未删除，
但退役标记不会再改变新版入口。

下一次在线需分别确认：当前云端版本能否触摸进入；进入后右摇杆和菜单是否响应；
修改游戏原生键位后退出并重新进入是否保留。任何一项都不能仅凭离线通过判为完成。
