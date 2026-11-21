# 0.2.130 终端 3 明文媒体接收

日期：2026-11-10。

## 结论

**终端 3（Android APK）能出画面了。** yuzu 实测：1280×720，403 帧解码，音频 758,400 采样全部播放。

这次不是猜出来的：日志里的 RTP/RTCP 字节直接证明了云端 pod 的行为。

## 根本原因

终端 3 的 pod 在 SDP 里声明 `m=video 9 UDP/TLS/RTP/SAVPF`（本该走 SRTP），DTLS 握手也照做，**但媒体流是明文 RTP/RTCP，完全没加密**。

证据链：

1. RTP 头是明文（这本来就正常，SRTP 不加密头），但载荷是合法的 H.264：

```
payload=78 00 1c 67 4d 40 20 95 a0 14 01 6e
        │  │     └── 67 4d 40 20 = H.264 SPS（profile 0x4d = Main）
        │  └── 00 1c = NAL 长度 28
        └── 78 = STAP-A（NAL type 24）

payload=7c 81 e0 42 13 ff fa 58 00 00 03 00
        │  └─ 81 e0 = FU-A start + NAL type 1
        └── 7c = FU-A（type 28）
```

`00 00 03` 是 H.264 防竞争字节，随机数据不会出现这个模式。

2. RTCP 60 字节 = SR(32) + SDES(28)，尾部是 ASCII：

```
a0 c8 00 07 ... 81 ca 00 06 ... 4c 75 48 42 64 7a 66 56 31 6e 6c 45
                                     └─ "LuHBdzfV1nlE"
```

这个字符串正是 SDP 里的 `a=ssrc:1280005769 cname:4hLuHBdzfV1nlE23`。SRTCP 加密后不可能解出可读字符串。

3. 0.2.129 的 6 个密钥布局探针全部为 0：

```
RTC_SRTP_PROBE_SHA1_32        0
RTC_SRTP_PROBE_SWAP_80        0
RTC_SRTP_PROBE_SWAP_32        0
RTC_SRTP_PROBE_AUTH_ONLY      0
RTC_SRTP_PROBE_MIX_KEY_SALT   0
RTC_SRTP_PROBE_MIX_SALT_KEY   0
```

如果 pod 用任何 SRTP 布局加密，总有一个能验证通过。全为 0 只可能是因为数据根本没加密。

## 修复方式

不猜测、不降级安全边界，用**结构验证 + 连续确认**判定明文：

- `plaintext_media_rtp_valid`：RTP 头合法，且 H.264 载荷结构合法（单 NAL / FU-A / FU-B / STAP-A 长度自洽）
- `plaintext_media_rtcp_valid`：RTCP compound 每个块的长度字段首尾相接，最后一个块正好落在数据报边界

然后要求 **8 个连续、序号递增的视频包**都通过结构验证才切换，或者 RTCP 连续 2 次通过加上视频侧至少 3 次结构通过。误判概率极低：错误密钥解出来的随机字节不可能连续 8 次凑出合法 H.264 帧结构。

切换后：

- 出站 RTP/RTCP 也发明文（pod 不解密媒体，也就不会接受加密反馈）
- 如果视频载荷连续 30 次不再符合 H.264 结构，自动撤销切换，回到 SRTP 路径重新判断
- 新的远端描述会清空切换状态，重新验证
- DTLS 握手、证书固定保持不变

`plaintext_outbound` 只影响媒体加解密，不影响 DTLS 应用数据（SDK 登录仍走加密数据通道）。

## 实测数据（yuzu，终端 3，真实账号）

```
STREAM_TERMINAL_TYPE              3
GAME_CONFIG_XBOX                  1     pod 侧认手柄
GAME_CONFIG_PS                    1
GAME_CONFIG_GR0006                1
INPUT_SOURCE_DECLARED             12    触摸 + 手柄
RTC_MEDIA_PLAINTEXT_ACTIVE        1
RTC_MEDIA_PLAINTEXT_RTP_STREAK    1055  连续通过
RTC_RTP_VIDEO_ROUTED              1048
RTC_RTP_AUDIO_ROUTED              790
VIDEO_UNITS_RECEIVED              421
VIDEO_DECODED_FRAMES              403
VIDEO_DECODE_ERRORS               0
VIDEO_FRAME_WIDTH                 1280
VIDEO_FRAME_HEIGHT                720
VIDEO_RENDER_SUBMISSIONS          1361
AUDIO_SAMPLES_PLAYED              758400
```

对比 0.2.128（同一终端、同一账号）：

```
RTC_RTP_DECRYPT_FAILURES    28706
VIDEO_DECODED_FRAMES            0
FIRST_VIDEO_TIMEOUT
```

## 顺带修掉的日志缺陷

恢复过程中发现三处日志格式串少了空格，会拼出 `datachannel=%dversion=%s` 这种不可读的输出。已修正：

- `dtls_done attempts=%d datachannel=%d version=%s cipher=%s`
- `udp_class=rtcp count=%d bytes=%d e14=%d e8=%d preview=%s tail=%s`
- `rtcp_decrypt_failed failures=%d bytesAfter=%d plainShape=%u`

## 新增诊断事件

| 事件 | 含义 |
|---|---|
| `RTC_MEDIA_PLAINTEXT_ACTIVE` | 是否已切换到明文媒体接收 |
| `RTC_MEDIA_PLAINTEXT_RTP_STREAK` | 视频明文结构连续通过次数 |
| `RTC_MEDIA_PLAINTEXT_RTP_PROMOTIONS` | 由视频结构确认切换的次数 |
| `RTC_MEDIA_PLAINTEXT_RTCP_PROMOTIONS` | 由 RTCP 结构确认切换的次数 |
| `RTC_RTP_VIDEO_DECRYPT_FAILURES` | 视频 RTP 认证失败次数 |
| `RTC_RTP_AUDIO_DECRYPT_FAILURES` | 音频 RTP 认证失败次数 |
| `RTC_RTP_VIDEO_DECRYPT_SUCCESSES` | 视频 RTP 认证成功次数 |
| `RTC_RTP_AUDIO_DECRYPT_SUCCESSES` | 音频 RTP 认证成功次数 |
| `RTC_SRTP_PROBE_*` | 6 种密钥布局探针（0.2.129 引入，全部为 0 即证明未加密） |

## 尚未验证

以下还需要真机确认：

- 键位持久化（0.2.127 已在 yuzu 验证 3 条映射回传一致，但需在出画面后确认游戏内实际生效）
- 手柄摇杆镜头、菜单键
- 触摸 + 手柄热插拔

## 文件

- 新增 `extern/libpeer/src/plaintext_media.h`：结构判定（纯函数，可独立测试）
- `extern/libpeer/src/peer_connection.c`：切换状态机、按媒体分开的计数
- `extern/libpeer/src/dtls_srtp.h` / `.c`：`plaintext_outbound` 出站旁路
- `app/src/mihoyo/rtc_session.cpp`：新事件上报
- `tests/peer_dtls_setup_test.c`：结构判定用例（真实 STAP-A/FU-A/RTCP 样本 + 拒绝用例）
