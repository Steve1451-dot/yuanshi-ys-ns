# 0.2.117 本地构建与验证

日期：2026-11-09（Asia/Singapore）。基于本工作区累计修改构建。
本轮没有提交、推送或创建 GitHub Release。

## 主机回归

8 个测试目标全部通过：

- `genshin_native_config`：原生键位表数量匹配、只改
  `enableInputHotSwap`、URI 编解码、无效／旧格式原样返回、SD 原文件不变。
- `mihoyo_sdk_large_config`：大配置逐字节回传、命令级大小边界、
  `cloud_get_data` 运行时补丁、ACK 可选字段兼容。
- `mihoyo_sdk_bridge`：Webview 全局 UA 约定及 SDK 桥回归。
- `mihoyo_game_control`：输入路由与手柄声明回归。
- `input_transport_policy`：RTC／GCC 选择与切换代次。
- `gyro_settings`、`gyro_stick_assist`：体感设置与体感转右摇杆。
- `mihoyo_ui_language`：界面文案。

主机版 Mbed TLS 加密库由 `tests/dtls_host` 重新构建后用于上述 SDK 测试。
离线测试不证明云端游戏采用 `enableInputHotSwap=true`，也不证明 E/Z/Q 已消失。

## Switch 构建

- `scripts/build-switch-msys2.sh`：devkitPro / devkitA64 Release 编译、链接、NRO 打包成功。
- `verify-cloudgenshin-version.sh` 在构建末尾确认 NRO 内含 `0.2.117`。
- 元数据检查通过：NRO0、ASET、NACP 0x4000、16 语言槽全部填充、
  标题 `yuanshi launcher`、作者 `Steve1451-dot`、显示版本 `0.2.117`。
- 第三方 stb_image_write 的既有缺省字段初始化警告不影响本次构建完成。

最终 NRO：30342865 字节。

SHA256：`C463E73C74B1804F99BB7499B3E9FE691AEE0F5D22A9CD6F8325A8820B0E4AB6`

## 尚未验收

本轮没有主动运行本版实机或模拟器游戏。以下项目仍待实机日志确认：

- `GAME_SDK_NATIVE_CONTROLLER_MAP_COUNT` 是否出现及数量是否与保存配置一致。
- `GAME_SDK_NATIVE_INPUT_HOTSWAP_REQUESTED`、`..._ALREADY_ENABLED`、
  `..._MODE_NOT_PATCHED` 中实际出现哪一项。
- 游戏是否接受该选项并进入手柄模式，E/Z/Q 键鼠提示是否消失。
- 原生自定义按键是否按保存的表生效。

安装包不含账号、运行日志、登录库、SDK 用户配置、密钥、二维码或测试标记。
