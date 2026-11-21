# 0.2.131 虚拟手柄身份修正

日期：2026-11-10。

## 问题

0.2.130 出画面后，游戏界面已经显示 Xbox 手柄按键提示（右下角 X/Y/A/B，右上 LB/RB，
右下 LT/RT），说明 pod 已下发 `gamepad_enable_xbox=1`。但探针实测：

- 推右摇杆（15–17s）镜头完全不动
- 按 Plus（20–20.2s）菜单不打开
- 6 张探针帧的小地图像素完全一致

日志显示输入帧确实发出：

```
t=75s GAMEPAD_LIST_REANNOUNCED_ON_FIRST_PRESS  1118/654
t=75s INPUT_LOCAL_PRESS_SAMPLED                 1
t=95s GAMEPAD_SERVER_MODE_NOT_REPORTED_20S      0
```

而 `SERVER_GAME_CONTROL_MODE`（pod 侧切换成功的回执，命令 20037）**在所有历史运行中
从未出现过**——说明 pod 一直没有真正创建虚拟手柄。

## 根因

APK 7.1.0 的 `com.mihoyo.cgsdk.GamepadManager.isGamepadOnAllowList`：

```java
private boolean isGamepadOnAllowList(int vendorId, int productId) {
    if (vendorId == 1638 && productId == 2184) return false;   // 0x0666/0x0888 排除
    if (vendorId > 0) { /* 见下 */ }
    if (vendorId == 1118) {                                    // 0x045E 微软
        if (productId == 736 || productId == 765 || productId == 2821 ||
            productId == 2835 || productId == 2850) return true;
    } else if (vendorId == 1356) {                             // 0x054C 索尼
        if (productId == 1476 || productId == 2508 || productId == 3302) return true;
    } else if (vendorId == 1133 && productId == 2809) {         // 罗技 GR0006
        return true;
    }
    return false;
}
```

官方只接受 Xbox `0x045E` 配这五个产品号：

| productId | 十六进制 | 型号 |
|---|---|---|
| 736 | 0x02E0 | Xbox One 蓝牙 |
| 765 | 0x02FD | Xbox One S 蓝牙 |
| 2821 | 0x0B05 | Xbox One 适配器 |
| 2835 | 0x0B13 | Xbox One S |
| 2850 | 0x0B22 | Xbox 无线适配器 |

我们此前一直用 **654 = 0x028E（Xbox 360）**，不在这张表里。官方 `USB_LIST` 也只配了
一个条目：`{1118 → {2816}}`，即 `0x045E → 0x0B00`。

pod 收到 `gamepad_list` 后能凭 vendor 判定"这是 Xbox"（所以 UI 换了手柄图标），
但设备不在允许列表里，虚拟手柄不会被创建，type-17 状态帧因此全部被丢弃。

## 修复

`app/src/mihoyo/input_packet.hpp`：

```cpp
constexpr std::uint32_t kXboxVendorId = 0x045e;
constexpr std::uint32_t kXboxProductId = 0x0b13;   // 原为 0x028e
```

选 `0x0B13`（Xbox One S）而不是 `0x0B00`：`0x0B00` 只在 `USB_LIST` 里作为"已插入
但未识别"的兜底，`0x0B13` 同时出现在允许列表，且 Xbox One 报告布局与我们的
buttons/thumb 编码一致。

新增回归断言（`tests/mihoyo_input_packet_test.cpp`）把允许列表写进测试，防止以后
再改回表外 ID：

```cpp
const std::uint32_t allowed[] = {0x02e0, 0x02fd, 0x0b05, 0x0b13, 0x0b22};
```

## 已核对且确认无需改动的部分

对照 `GamepadInstance.getXinputData()`，我们的编码逐位一致：

| 项 | 官方 | 我们 |
|---|---|---|
| D-pad 上/下/左/右 | 1/2/4/8 | 1/2/4/8 |
| Start / Back | 16 / 32 | 16 / 32 |
| 左右摇杆按下 | 64 / 128 | 64 / 128 |
| LB / RB | 256 / 512 | 256 / 512 |
| A / B / X / Y | 4096 / 8192 / 16384 / 32768 | 同 |
| Y 轴方向 | `* -32767`（取反） | 取反 |
| 摇杆缩放 | `* 32767` | 同 |

数据类型与字段也一致（协议文档 §1）：

```
17 GAMEPAD_KEY   → RtcDataChannel field 10
18 GAMEPAD_LIST  → RtcDataChannel field 11
21 TOUCH_EVENT_LIST → RtcDataChannel field 13
```

## 尚待实测

本次只改了身份，需要 yuzu 实测确认：

- `SERVER_GAME_CONTROL_MODE` 是否终于出现（这是 pod 侧成功的直接回执）
- 右摇杆是否转动镜头
- Plus 是否打开菜单

如果仍然没有 20037，说明 pod 创建虚拟手柄还依赖别的条件（例如客户端侧的
`gamepad_list` 时机，或 pod 的 Android 输入注入服务），届时再按证据推进。

## 文件

- `app/src/mihoyo/input_packet.hpp`：手柄身份常量
- `tests/mihoyo_input_packet_test.cpp`：允许列表断言 + packed 编码断言
- `app/src/app_version.hpp`：0.2.131
