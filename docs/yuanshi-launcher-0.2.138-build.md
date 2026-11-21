# 0.2.138 本地构建与验证

日期：2026-11-13（Asia/Singapore）。当前为排障 alpha，不是完整可玩验收。

## 已完成

- devkitA64 Release 完整构建后完成最终增量编译、链接、NRO 打包。
- NRO0 / ASET / NACP 校验通过，版本 0.2.138、作者、图标与 16 个语言槽匹配。
- 最终 NRO：30,400,209 字节。
- SHA256：`D6C2FC3E10B9C500B5CC51B4EBC20701C09EFD901692C8B05CB89D703858C159`。
- 本轮相关文件 `git diff --check` 通过；未清理无关工作区改动或旧版本产物。
- 未提交、推送、创建 GitHub Release 或复制到真实 Switch。

## 离线测试

以下 14 个主机目标通过：

`genshin_native_config`、`mihoyo_sdk_large_config`、`mihoyo_sdk_bridge`、
`mihoyo_sdk_cloud_data_store`、`mihoyo_input_packet`、`mihoyo_rtc_session`、
`mihoyo_autotest_pad_policy`、`mihoyo_ui_language`、`mihoyo_stream_input_policy`、
`mihoyo_launcher_input_policy`、`idle_gamepad_policy`、`input_transport_policy`、
`gyro_stick_assist`、`controller_system_buttons`。

覆盖原样配置回包、三次独立应用生命周期的加密保存/恢复、真实 SDK 队列与加密、
原生输入编号范围隔离/独立计数/失败重试、UI 与网络过滤顺序下的探针状态、
触摸与手柄释放、重连中性帧、日志边沿去重与 32 条上限。

`cloudgenshin_monitor_test.ps1` 四组测试通过，包括 Exact/Off 安全证据保留。
这些测试使用合成账号与假传输，不连接云服务，不读取用户登录库。

输入报文的 field-18 编码已去除额外临时 vector 分配；按钮位和摇杆格式保持原契约。

## 真实验证边界

较早内部构建曾在 yuzu 进入实际游戏场景，但右摇杆/菜单探针未呈现预期响应。
详细证据及该内部产物哈希见本版说明。不能把“进入世界”“回包相同”或本机写成功
称为映射完整可玩。

用户要求先不连接后，已停止在线测试。最终上述哈希仅有离线与构建验证，
尚未验证最终手柄边沿日志、真实云端按键效果、Switch 重启保留和长时间运行。
待用户决定恢复联网测试后，再比较 Keybinds 与 Exact，检查最终发出的手柄状态。
