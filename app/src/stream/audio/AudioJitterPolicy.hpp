#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace opennow::audio {
// RFC 6716 section 3: duration metadata only, not an Opus decoder/validator.
// Work in the mandatory 48 kHz RTP clock, including mono/low-bandwidth Opus.
constexpr unsigned OpusPacketSamples48k(const std::uint8_t* bytes,std::size_t size) {
    if(!bytes || !size) return 0;
    const unsigned config=bytes[0]>>3,code=bytes[0]&3;
    constexpr unsigned silk[]{480,960,1920,2880};
    constexpr unsigned hybrid[]{480,960};
    constexpr unsigned celt[]{120,240,480,960};
    const unsigned frame=config<12?silk[config%4]:config<16?hybrid[config%2]:celt[config%4];
    if(code==3 && size<2) return 0;
    const unsigned count=code==0?1:code==3?(bytes[1]&63):2;
    return count && frame*count<=5760?frame*count:0;
}

// Never chase a moving live edge with seconds of fabricated silence. Preserve
// short reorder/loss repair, then resume the real packet already in the queue.
class GapConcealBudget {
public:
    bool allow(unsigned samples) {
        if(!samples || samples>1440 || samples_+samples>1440) return false;
        samples_+=samples;return true;
    }
    void reset(){samples_=0;}
private:
    unsigned samples_=0; // At most 30 ms, independent of packet duration.
};
constexpr unsigned AudioQueueHighSamples(int target_ms) {
    return static_cast<unsigned>(std::clamp(target_ms,30,100)+20)*48;
}
constexpr int RecoverTargetMs(int target_ms,bool adaptive) {
    return std::clamp(target_ms+(adaptive?20:0),30,100);
}
} // namespace opennow::audio
