# 一键传 NRO 到 Switch（保留全部旧版）

在 Switch 打开 USB MTP 文件传输，连接电脑并保持该界面。双击项目根目录的
`send-to-switch.bat` 即可，不需要管理员权限或安装额外工具。

脚本从本项目 `dist` 自动选择版本号最高的 `yuanshi-launcher-X.Y.Z.nro`，先验证
对应 `.nro.sha256.txt`、NRO/NACP、产品名和内嵌版本，再把该文件复制到：

```text
此电脑 / Nintendo Switch / microSD card / switch /
    yuanshi-launcher-X.Y.Z.nro
```

不删除或覆盖任何 Switch 文件，不触碰账号、配置、日志或旧程序。同名且 SHA256
相同则直接跳过；同名但内容不同则为新文件添加 SHA256 前八位后缀。复制后从
Switch 读回新 NRO，完整比对长度及 SHA256，只有一致才打印 `READ-BACK SHA256 MATCH`。
遇到断连、重名歧义、损坏/未打包的最新版本、超时或校验不符时明确失败，不谎报成功。

更新后在 hbmenu / sphaira 选择**新版本号**的 NRO。脚本不会更新 HOME 转发器
固定路径，也不会整理或隐藏旧版本；旧版仍然可以作为回退使用。

仅预检查，不往 Switch 写文件：

```bat
send-to-switch.bat -CheckOnly
```

也可把带版本号且附 SHA256 文本的 NRO 拖到 BAT，或明确指定：

```bat
send-to-switch.bat "D:\packages\yuanshi-launcher-0.2.145.nro"
```

缺省等待 USB MTP 15 秒，传输验证限时 90 秒；可使用
`-WaitForDeviceSeconds 30 -TimeoutSeconds 120` 调整。BAT 窗口结束前会暂停便于
查看结果；脚本调用时可设置 `YUANSHI_DEPLOY_NO_PAUSE=1`。执行策略仅为该子进程的
`RemoteSigned`，不修改电脑持久执行策略、TLS 校验或系统时间。

电脑上只创建本次专用临时 NRO 读回文件，完成后清理这些本地副本；从不在 Switch
上执行删除。源文件和 SHA256 文本保持不变。
