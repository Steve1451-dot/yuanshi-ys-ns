# 0.2.113 本地构建与验证

日期：2026-11-07（Asia/Singapore）。基于 GitHub main
`0d9af76eba6d458879b3f8f8cdbb02c155cd9920`，包含本轮尚未提交的本地修改。
未创建 GitHub Release，未改写远程提交历史。

## 已完成

- `build-switch.ps1`：devkitPro / devkitA64 Release 构建与 NRO 打包成功。
  沙箱下 MSYS2 IPC 初始化被拦截后，经权限通道构建；不是主机编译冒充 Switch 构建。
- 最终变更重新增量编译、链接、打包成功。
- `tests/yuanshi_nro_metadata_test.ps1`：NRO0、ASET、NACP、16 个语言槽、
  版本 0.2.113、作者 Steve1451-dot 与所选图标一致。
- 12 项主机回归通过：`compact_stream_menu`、`controller_system_buttons`、
  `endfield_xinput_state`、`genshin_input_experiment`、`gyro_settings`、
  `gyro_stick_assist`、`mihoyo_rtc_session`、`mihoyo_safe_event_log`、
  `mihoyo_sdk_bridge`、`mihoyo_sdk_cloud_data_store`、`mihoyo_sdk_json`、
  `mihoyo_ui_language`。
- 最后增加 SD 文件路径碰撞保护后，重新运行 SDK bridge / store 两项测试并通过。
  覆盖两个会话的 SDK 编解码与精确恢复、后台保存、最新覆盖、账号／渠道隔离、
  密钥与快照损坏恢复、认证失败、非法键／值／超限拒绝和目录碰撞保护。
- `git diff --check` 通过。旧改键模块和编辑器已移除；没有读取或删除用户的旧配置。

主机会话测试中的 fake transport error 是离线故障注入用例，不是线上登录或串流失败。
第三方 stb_image_write 编译中已有缺省字段初始化警告，不影响本次构建成功。

## 最终 NRO

- 字节数：30330577
- SHA256：`3D326C3DC0AF3F469A1F0AE6C22EC83BE4C9E8E0C058A2D89336AE9B676E5F6B`

本包不含登录库、Cookie、二维码、账号令牌或测试会话数据。

## 未验收

本轮没有运行新的真实 Switch 登录、排队或游戏测试，也未用 yuzu 冒充实机性能验证。
游戏自身的 opaque SDK 配置能否包含并恢复手柄动作绑定，仍需一次完整的真实重启实验。
测试通过不代表已证明键位永久保存，也不等于可以继承云主机内存。
