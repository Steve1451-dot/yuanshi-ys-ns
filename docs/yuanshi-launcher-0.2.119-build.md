# 0.2.119 ?????????? 0.2.120 ???

> ???? Keybinds ??????????????????????
> ? [0.2.120 ????](yuanshi-launcher-0.2.120.md)?????????????????

# 0.2.119 本地构建与验证

日期：2026-11-09（Asia/Singapore）。基于本工作区累计修改构建。
本轮没有提交、推送或创建 GitHub Release。

## 主机回归

完整 `scripts/test-mihoyo-host.ps1` 测试集全部通过。与本版直接相关的目标：

- `genshin_native_config`：键位模式保留手柄映射、过滤 `KeyboardMap`、
  只改已存在的三个输入模式布尔字段；原始存档不变；无手柄映射时返回空；
  Full／Off 模式与开关文件（LF／CRLF／无换行、未知内容）行为符合预期。
- `mihoyo_sdk_large_config`：键位模式返回过滤后的配置，不再触发旧的
  `INPUT_HOTSWAP_REQUESTED`；解析失败时 `cloud_get_data` 返回空，
  绝不回退原始完整配置；大配置逐字节回传与大小边界回归通过。
- `mihoyo_sdk_bridge`：Webview 全局 UA 约定、SDK 桥及“只保存不回传”回归通过。
- `mihoyo_ui_language`：新增键位模式与保存诊断文案断言通过。
- 其余输入路由、RTC／GCC、音频、解码、登录、存储与界面策略测试均通过。

离线测试只证明过滤与回传逻辑，不证明云端游戏采用键位，也不证明输入模式保持。

## Switch 构建

- `scripts/build-switch-msys2.sh`：devkitPro / devkitA64 Release 编译、链接、NRO 打包成功。
- `verify-cloudgenshin-version.sh` 在构建末尾确认 NRO 内含 `0.2.119`。
- 元数据检查通过：NRO0、ASET、NACP 0x4000、16 语言槽全部填充、
  标题 `yuanshi launcher`、作者 `Steve1451-dot`、显示版本 `0.2.119`。
- 第三方 stb_image_write 的既有缺省字段初始化警告不影响本次构建完成。

最终 NRO：30355153 字节。

SHA256：`6689632ABC12D7BA714609DC9BC17D95E754D0947303768C183E536E2056E5E7`

## 实机验证

本版尚未实机验证。0.2.118 已确认的基线是：已有保存配置时默认不回传，
实体手柄正常；但两次会话中游戏都没有请求 `cloud_get_data`。

0.2.119 需要一次“游戏确实请求配置”的会话，并同时确认：

- `GAME_SDK_NATIVE_CONFIG_KEYBINDS_ON`；
- `GAME_SDK_NATIVE_SAVED_CONFIG_PRESENT` 及保存配置的映射／开关数量；
- `GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT`；
- `GAME_SDK_NATIVE_KEYBOARD_MAP_COUNT` / `..._INPUT_MODE_FIELDS_CHANGED`；
- 进入游戏后自定义键位仍在，界面仍是手柄模式。

如果游戏整场不请求配置，本版不会执行恢复；这不能算成功，也不能算失败。

## 尚未覆盖

- 键位模式在真实 `GENERAL_DATA` 上的字段名与键盘映射格式尚未逐一核对。
- `enableInputHotSwap=false`、`rewiredEnableKeyboard=false`、
  `rewiredDisableKeyboard=true` 的组合是否最适合云端输入模式，仍需实机 A/B。
- 完整配置回传（`CGNX_NATIVE_CONFIG_RESTORE_V1`）未再复测。
- bt 的 `1001_2` 与本版无关，仍未解决。

安装包不含账号、运行日志、登录库、SDK 用户配置、密钥、二维码或测试标记。
