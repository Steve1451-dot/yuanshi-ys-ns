#pragma once
#include <cstddef>
#include <cstdint>

namespace cloudgenshin::sdk {
inline constexpr std::uint16_t kReliableMessageQueueCmd = 1900;
inline constexpr std::size_t kMaximumSdkPlaintextBytes = 256 * 1024;
// AES padding + base64 + protobuf envelopes are larger than plaintext.
// Official RmqSender sends one message with seqId=seqCnt=1 over TCP/WebSocket;
// Packet's body length is uint32, not uint16. Keep a separate finite wire cap.
inline constexpr std::size_t kMaximumSdkQueueBytes = 512 * 1024;
} // namespace cloudgenshin::sdk
