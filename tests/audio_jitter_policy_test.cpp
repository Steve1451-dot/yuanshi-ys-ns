#include "stream/audio/AudioJitterPolicy.hpp"
#include <array>
#include <cassert>

int main() {
    using namespace opennow::audio;
    assert(!OpusPacketSamples48k(nullptr,0));
    const std::array<unsigned,32> expected{
        480,960,1920,2880,480,960,1920,2880,480,960,1920,2880,
        480,960,480,960,120,240,480,960,120,240,480,960,120,240,480,960,120,240,480,960};
    for(unsigned config=0;config<32;++config) {
        std::uint8_t bytes[]{static_cast<std::uint8_t>(config<<3),0};
        assert(OpusPacketSamples48k(bytes,2)==expected[config]);
        bytes[0]|=1;assert(OpusPacketSamples48k(bytes,2)==expected[config]*2);
        bytes[0]|=2;bytes[1]=2;
        assert(!OpusPacketSamples48k(bytes,1));
        assert(OpusPacketSamples48k(bytes,2)==expected[config]*2);
        bytes[1]=0;assert(!OpusPacketSamples48k(bytes,2));
        bytes[1]=63;assert(!OpusPacketSamples48k(bytes,2));
    }
    std::uint8_t full[]{0x1b,2};assert(OpusPacketSamples48k(full,2)==5760); // 120 ms, not fixed 10.
    GapConcealBudget gap;
    for(int i=0;i<3;++i) assert(gap.allow(480));
    assert(!gap.allow(480));gap.reset();
    assert(gap.allow(960) && !gap.allow(960));gap.reset();
    assert(!gap.allow(5760) && !gap.allow(0));
    assert(AudioQueueHighSamples(30)==2400 && AudioQueueHighSamples(60)==3840);
    assert(RecoverTargetMs(60,true)==80 && RecoverTargetMs(80,true)==100 && RecoverTargetMs(100,true)==100);
    assert(RecoverTargetMs(40,false)==40);
}
