#pragma once
#include "mihoyo/input_packet.hpp"
#include <cstdint>
#include <string_view>

namespace cloudgenshin::rtc {
enum class InputTransport {Unavailable, WebControl, Rtc};

// A paired real-device run accepted forced Xbox input through RTC, but not
// through a newly available GCC socket. Scope this preference to that native
// ys route; do not change officially enabled pads, PS identities or other apps.
inline bool PreferNativeGamepadRtc(std::string_view app, std::uint32_t vendor,
                                   std::uint32_t product, bool server_pad_enabled) {
    return app=="4" && vendor==input::kXboxVendorId && product==input::kXboxProductId && !server_pad_enabled;
}
inline InputTransport SelectInputTransport(bool web_ready,bool rtc_ready,bool prefer_rtc) {
    if(prefer_rtc && rtc_ready) return InputTransport::Rtc;
    if(web_ready) return InputTransport::WebControl;
    return rtc_ready ? InputTransport::Rtc : InputTransport::Unavailable;
}
class InputTransportEpoch {
public:
    bool observe(InputTransport next) {
        if(next==kind_) return false;
        kind_=next;++generation_;return true;
    }
    InputTransport kind() const {return kind_;}
    std::uint64_t generation() const {return generation_;}
    void reset() {kind_=InputTransport::Unavailable;generation_=0;}
private:
    InputTransport kind_=InputTransport::Unavailable;
    std::uint64_t generation_=0;
};
inline bool RedeclareInput(bool ready,bool previously_ready,std::uint64_t previous_epoch,
                           std::uint64_t epoch,bool game_entry) {
    return ready && (!previously_ready || previous_epoch!=epoch || game_entry);
}
} // namespace cloudgenshin::rtc
