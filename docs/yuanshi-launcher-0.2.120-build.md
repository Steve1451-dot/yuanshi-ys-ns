# 0.2.120 本地构建与验证

日期：2026-11-09（Asia/Singapore）。基于本工作区累计修改构建。
本轮没有提交、推送或创建 GitHub Release。

## 修订原因

审查发现 0.2.118 的“run2 整场没有请求配置”结论是被截断的快照日志误导：
`cloudgenshin-safe.log` 第 131 行 `omitted_event_count=3352` 丢掉了 t≈5s–105s，
而 `stream-logs/stream-safe-01.log` 证明 t=14s 确实读了配置、t=15s 被 Off 跳过。
因此 0.2.119 的默认 Keybinds 缺少实机依据，本版回退为 Off。

## 主机回归

`scripts/test-mihoyo-host.ps1` 完整测试集通过。直接相关目标：

- `genshin_native_config`：M0 只返回两个映射数组；排除键盘／鼠标／触屏映射；
  未知根元素但形似映射的条目保留并计数；非映射值丢弃；空条目逐条丢弃；
  超 64 条整次放弃；幂等性 `filter(filter(x)) == filter(x)`；原因码；
  结构报告只含名称／类型／数量／根元素名；默认 Off 与 BOM 未知标记。
- `mihoyo_safe_event_log`：新增证据事件在 2600 条计数与字节预算下仍保留、
  保持行号顺序；增量归档在环形淘汰后补发。
- `mihoyo_sdk_large_config`：M0 回包只含映射；解析失败返回空；
  游戏回写映射与回传一致时输出采用证据。
- `mihoyo_sdk_bridge`、`mihoyo_sdk_cloud_data_store`、`mihoyo_safe_log_archive`、
  `mihoyo_ui_language`：回归通过。
- 其余输入路由、RTC／GCC、音频、解码、登录与界面策略测试通过。

`assert` 型测试使用 `-std=c++20 -Wall -Wextra -Werror`，脚本未定义 `NDEBUG`，
断言不会被编译掉。

离线测试不证明云端游戏接受 M0 负载，也不证明输入模式保持。

## 最终产物

最终 NRO：30379729 字节。

SHA256：`12DCA23184F85643008C7B760CDB32A03CE5651FFFB5336D91A6C3CDE4D665E1`

分发包：见 `dist/yuanshi-launcher-0.2.120-SHA256SUMS.txt`（zip 内字节随文档更新变化，
以该文件为准；包内 NRO 始终与上面的 NRO 哈希一致）。

- NRO0 / ASET / NACP 0x4000 校验通过；
- 16 个语言槽全部填充，标题 `yuanshi launcher`、作者 `Steve1451-dot`；
- 显示版本 `0.2.120`（构建末尾 `verify-cloudgenshin-version.sh` 同时确认）；
- 包内 NRO 与构建产物逐字节一致；不含 key／vault／日志／快照／结构报告。

## 尚未覆盖

- M0 负载在真实 `GENERAL_DATA` 上是否被接受：需要实机或解密核对。
- 真实存档字段名与映射根元素格式：需要先解密核对。
- `enableInputHotSwap` 等开关的真实语义：本版不再猜测或强制写入。
- 完整回传 A/B（`CGNX_NATIVE_CONFIG_RESTORE_V1`）：未复测。
- bt 的 `1001_2` 与本版无关，仍未解决。

安装包不含账号、运行日志、登录库、SDK 用户配置、密钥、二维码或测试标记。
