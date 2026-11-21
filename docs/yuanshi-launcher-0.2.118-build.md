# 0.2.118 本地构建与验证

日期：2026-11-09（Asia/Singapore）。基于本工作区累计修改构建。
本轮没有提交、推送或创建 GitHub Release。

## 主机回归

9 个测试目标全部通过：

- `genshin_native_config`：原生键位表数量匹配、只改
  `enableInputHotSwap`、URI 编解码、无效／旧格式原样返回、SD 原文件不变；
  新增实验开关的 LF／CRLF／无换行均接受、额外内容拒绝测试。
- `mihoyo_sdk_large_config`：大配置逐字节回传、命令级大小边界、
  `cloud_get_data` 运行时补丁、ACK 可选字段兼容。
- `mihoyo_sdk_bridge`：Webview 全局 UA 约定及 SDK 桥回归；新增“只保存不回传”
  用例，确认 `cloud_get_data` 返回空、跳过事件出现、部分更新不会抹掉其他键。
- `mihoyo_game_control`：输入路由与手柄声明回归。
- `input_transport_policy`：RTC／GCC 选择与切换代次。
- `gyro_settings`、`gyro_stick_assist`：体感设置与体感转右摇杆。
- `mihoyo_ui_language`：界面文案，含新增的配置回传开关事件。
- `mihoyo_sdk_cloud_data_store`：加密存储、账号／渠道隔离、原子写入与恢复；
  新增的“只保存”事件不影响原有恢复行为。

主机版 Mbed TLS 加密库由 `tests/dtls_host` 重新构建后用于上述 SDK 测试。
离线测试不证明 E/Z/Q 已消失，也不证明手柄已控制云端游戏。

## Switch 构建

- `scripts/build-switch-msys2.sh`：devkitPro / devkitA64 Release 编译、链接、NRO 打包成功。
- `verify-cloudgenshin-version.sh` 在构建末尾确认 NRO 内含 `0.2.118`。
- 元数据检查通过：NRO0、ASET、NACP 0x4000、16 语言槽全部填充、
  标题 `yuanshi launcher`、作者 `Steve1451-dot`、显示版本 `0.2.118`。
- 第三方 stb_image_write 的既有缺省字段初始化警告不影响本次构建完成。

最终 NRO：30342865 字节。

SHA256：`484FCFA3424F1687152FA46115AB3251E91F49E8FDC799DB3DBE4B689466D73E`

## 实机验证

2026-11-09 两次进入原神（同一 Switch、同一账号、同一 Xbox 手柄）：

- 第一次：重新扫码登录，`GAME_SDK_CLOUD_DATA_EMPTY`；`RESTORE_OFF` 出现；
  游戏写入后 `SAVE_ONLY` / `STORED` / `PERSISTED` 均出现；手柄正常。
- 第二次：`GAME_SDK_CLOUD_DATA_VAULT_READY code=1` 与 `RESTORE_OFF` 同时出现；
  没有 `LARGE_REPLY_SENT`、`INPUT_HOTSWAP_REQUESTED`、`CLOUD_DATA_RESTORED`；
  手柄正常。
- 第二次游戏未请求该缓存键，因此没有 `RESTORE_SKIPPED`；配置确实只保存在
  内存／SD，未回传。
- 第一次 t=62s 的 `GAMEPAD_SERVER_MODE_NOT_REPORTED_20S` 与手柄实际可用矛盾，
  说明该诊断不可靠，后续版本调整。

## 尚未覆盖

- 打开 `cloudgenshin-native-config.txt` 实验开关后的 A/B 行为未再测试。
- 只恢复键位字段、不恢复输入模式的精细方案仍未实现；需要先确认
  `GENERAL_DATA` 中控制输入设备的字段。
- bt 的 `1001_2` 与本版无关，仍未解决。

安装包不含账号、运行日志、登录库、SDK 用户配置、密钥、二维码或测试标记。
