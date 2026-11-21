#include "stream/audio/AudioPipeline.hpp"
#include "stream/audio/AudioJitterPolicy.hpp"
#include "peer_connection.h"
#include <switch.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
std::mutex mutex;
std::condition_variable changed;
std::deque<AudioOutBuffer*> output;
std::vector<int> decode_generations;
std::vector<int> decoded_sequences;
std::vector<int> reclaim_queries_at_append;
int decoder_generation = 0;
int reclaim_queries = 0;
int starts = 0;
int stops = 0;
u64 played_samples = 0;
bool release_output = false;
int release_budget = -1;
bool output_started = false;
bool output_closed = false;
float output_volume = 0.0f;
bool fail_output_start = false;
bool diagnostics_enabled = false;
std::string diagnostic_home="test-only-missing-app-directory";

template <typename Predicate>
void await(Predicate predicate)
{
    std::unique_lock lock(mutex);
    assert(changed.wait_for(lock, std::chrono::seconds(2), predicate));
}

void submit_epoch(AudioPipeline& pipeline, uint32_t ssrc)
{
    uint8_t payload[] = {0xf0, 0x00};
    for (uint16_t sequence : {2, 0, 0, 1}) {
        payload[1] = static_cast<uint8_t>(sequence);
        PeerAudioPacket packet {};
        packet.data = payload;
        packet.size = sizeof(payload);
        packet.timestamp = 480 * sequence;
        packet.ssrc = ssrc;
        packet.sequence = sequence;
        packet.payload_type = 111;
        pipeline.submit(packet);
    }
}

template<class Predicate> void wait_audio(Predicate predicate) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!predicate()) {
        assert(std::chrono::steady_clock::now()<deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}

namespace opennow {
bool StreamDiagnosticsEnabled() { return diagnostics_enabled; }
const std::string& AppHomePath() {return diagnostic_home;}
}

Result hwopusDecoderInitialize(HwopusDecoder* decoder, int, int)
{
    std::lock_guard lock(mutex);
    decoder->generation = ++decoder_generation;
    return 0;
}
void hwopusDecoderExit(HwopusDecoder*) {}
Result hwopusDecodeInterleaved(HwopusDecoder* decoder, int32_t* consumed, int32_t* samples,
                              const void* input, size_t size, void* pcm, size_t capacity)
{
    std::lock_guard lock(mutex);
    *consumed = static_cast<int32_t>(size);
    *samples = static_cast<int32_t>(opennow::audio::OpusPacketSamples48k(
        static_cast<const uint8_t*>(input)+sizeof(HwopusHeader),size-sizeof(HwopusHeader)));
    assert(*samples>0 && capacity >= static_cast<std::size_t>(*samples) * 2 * sizeof(int16_t));
    std::memset(pcm, 0, static_cast<std::size_t>(*samples) * 2 * sizeof(int16_t));
    decode_generations.push_back(decoder->generation);
    assert(size == sizeof(HwopusHeader) + 2);
    decoded_sequences.push_back(static_cast<const uint8_t*>(input)[sizeof(HwopusHeader) + 1]);
    reclaim_queries = 0;
    changed.notify_all();
    return 0;
}
Result audoutInitialize() { return 0; }
void audoutExit() {
    std::lock_guard lock(mutex);
    assert(!output_started);
    output_closed = true;
    output.clear();
}
Result audoutSetAudioOutVolume(float volume) {
    std::lock_guard lock(mutex);
    output_volume = volume;
    return 0;
}
u32 audoutGetSampleRate() { return 48000; }
u32 audoutGetChannelCount() { return 2; }
PcmFormat audoutGetPcmFormat() { return PcmFormat_Int16; }
Result audoutGetAudioOutPlayedSampleCount(u64* samples)
{
    std::lock_guard lock(mutex);
    *samples = played_samples;
    return 0;
}
Result audoutGetReleasedAudioOutBuffer(AudioOutBuffer** buffer, u32* count)
{
    std::lock_guard lock(mutex);
    ++reclaim_queries;
    *buffer = nullptr;
    *count = 0;
    if (release_output && release_budget!=0 && !output.empty()) {
        *buffer = output.front();
        *count = 1;
        played_samples += output.front()->data_size / (2 * sizeof(int16_t));
        output.pop_front();
        if(release_budget>0) --release_budget;
    }
    return 0;
}
Result audoutStartAudioOut()
{
    std::lock_guard lock(mutex);
    if (fail_output_start) return 0x1234;
    output_started = true;
    ++starts;
    changed.notify_all();
    return 0;
}
Result audoutStopAudioOut()
{
    std::lock_guard lock(mutex);
    output_started = false;
    ++stops;
    changed.notify_all();
    return 0;
}
Result audoutFlushAudioOutBuffers(bool* flushed)
{
    std::lock_guard lock(mutex);
    assert(!output_started); // Regression: flush used to precede stop.
    output.clear();
    *flushed = true;
    return 0;
}
Result audoutAppendAudioOutBuffer(AudioOutBuffer* buffer)
{
    std::lock_guard lock(mutex);
    output.push_back(buffer);
    reclaim_queries_at_append.push_back(reclaim_queries);
    changed.notify_all();
    return 0;
}
Result audoutContainsAudioOutBuffer(AudioOutBuffer* buffer, bool* contains)
{
    std::lock_guard lock(mutex);
    *contains = std::find(output.begin(), output.end(), buffer) != output.end();
    return 0;
}
void armDCacheFlush(void*, size_t) {}

static void check_output_timeline()
{
    AudioPipeline pipeline;
    pipeline.configure(1000, 30);
    assert(pipeline.start());
    submit_epoch(pipeline, 1);
    await([] { return starts == 1; });
    pipeline.set_sender_report(1, 1000000, 0);
    auto send = [&](uint16_t sequence) {
        uint8_t payload[] = {0xf0, static_cast<uint8_t>(sequence)};
        PeerAudioPacket packet {};
        packet.data = payload;
        packet.size = sizeof(payload);
        packet.timestamp = 480 * sequence;
        packet.ssrc = 1;
        packet.sequence = sequence;
        packet.payload_type = 111;
        pipeline.submit(packet);
    };
    for (uint16_t sequence = 3; sequence < 9; ++sequence)
        send(sequence);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (pipeline.levels().packets_decoded!=5 || !pipeline.levels().pacing_waits) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(pipeline.playback_ntp_us() <= 1050000);
    {
        std::lock_guard lock(mutex);
        assert(output.size() == 5);
        release_output = true;release_budget=5;
    }
    wait_audio([&] { return pipeline.levels().samples_played==2400; });
    wait_audio([&] { return pipeline.levels().packets_decoded==9; });
    assert(pipeline.levels().output_drops==0); // The old implementation dropped four decoded packets here.
    assert(pipeline.playback_ntp_us() >= 1050000 && pipeline.playback_ntp_us() <= 1090000);
    {
        std::lock_guard lock(mutex);
        release_output = true;release_budget=4;
    }
    wait_audio([&] { return pipeline.levels().samples_played==4320; });
    {std::lock_guard lock(mutex);release_output=false;}
    for (uint16_t sequence = 9; sequence < 12; ++sequence)
        send(sequence);
    wait_audio([&] { return pipeline.levels().packets_decoded==12; });
    {
        std::lock_guard lock(mutex);
        release_output = true;release_budget=3;
    }
    wait_audio([&] { return pipeline.levels().samples_played==5760; });
    assert(pipeline.playback_ntp_us() == 1120000);
    pipeline.stop();
    {
        std::lock_guard lock(mutex);
        release_output = false;
    }
    assert(pipeline.start());
    submit_epoch(pipeline, 1);
    await([] { return starts == 2; });
    pipeline.set_sender_report(1, 2000000, 0);
    {
        std::lock_guard lock(mutex);
        release_output = true;release_budget=-1;
    }
    wait_audio([&] { return pipeline.levels().samples_played==1440; });
    assert(pipeline.playback_ntp_us() == 2030000);
    pipeline.stop();
}

static void check_ssrc_timeline()
{
    AudioPipeline pipeline;
    pipeline.configure(1000, 30);
    assert(pipeline.start());
    submit_epoch(pipeline, 1);
    await([] { return starts == 1; });
    pipeline.set_sender_report(1, 1000000, 0);
    submit_epoch(pipeline, 2);
    pipeline.set_sender_report(2, 3000000, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (pipeline.levels().packets_decoded!=5) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(pipeline.playback_ntp_us() == -1);
    {
        std::lock_guard lock(mutex);
        assert(output.size() == 5);
        release_output = true;release_budget=3;
    }
    wait_audio([&] { return pipeline.levels().samples_played==1440 && pipeline.levels().packets_decoded==6; });
    assert(pipeline.playback_ntp_us() >= 3000000 && pipeline.playback_ntp_us() <= 3030000);
    {std::lock_guard lock(mutex);release_budget=3;}
    wait_audio([&] {return pipeline.levels().samples_played==2880;});
    assert(pipeline.playback_ntp_us() == 3030000 && pipeline.levels().output_drops==0);
    pipeline.stop();
}

int main(int argc, char** argv)
{
    if(argc>1 && std::string(argv[1])=="diagnostics") {
        const auto root=std::filesystem::temp_directory_path()/
            ("yuanshi-audio-diag-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(root);diagnostic_home=root.string();diagnostics_enabled=true;
        {std::ofstream previous(root/"audio.log");previous<<"OLD_SESSION";}
        AudioPipeline pipeline;pipeline.configure(100,30);assert(pipeline.start());
        submit_epoch(pipeline,1);await([]{return starts==1;});
        pipeline.set_sender_report(1,1000000,0);pipeline.stop();
        std::ifstream file(root/"audio.log",std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(file),{}};file.close();
        assert(text.find("SESSION start")!=std::string::npos);
        assert(text.find("JITTER primed")!=std::string::npos);
        assert(text.find("RTCP_SR")!=std::string::npos && text.find("SUMMARY")!=std::string::npos);
        assert(text.find("OLD_SESSION")==std::string::npos);
        assert(pipeline.levels().diagnostic_write_errors==0);
        std::filesystem::remove(root/"audio.log");std::filesystem::remove(root);return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "output-failure") {
        fail_output_start = true;
        AudioPipeline pipeline;
        pipeline.configure(100, 30);
        assert(pipeline.start());
        submit_epoch(pipeline, 1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (pipeline.levels().output_errors == 0) {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto levels = pipeline.levels();
        assert(levels.output_ready && !levels.output_started);
        assert(levels.last_output_error == 0x1234 && levels.samples_played == 0);
        pipeline.stop();
        assert(output_closed && output_volume == 1.0f);
        return 0;
    }
    if (argc > 1) {
        const auto mode=std::string(argv[1]);
        const auto send=[](AudioPipeline& pipeline,uint16_t sequence,uint8_t toc=0xf0,unsigned samples=480) {
            uint8_t payload[]{toc,static_cast<uint8_t>(sequence)};
            PeerAudioPacket packet{};packet.data=payload;packet.size=sizeof(payload);
            packet.ssrc=1;packet.sequence=sequence;packet.timestamp=samples*sequence;packet.payload_type=111;
            pipeline.submit(packet);
        };
        if(mode=="backpressure" || mode=="overflow-live-edge") {
            AudioPipeline pipeline;pipeline.configure(100,30);assert(pipeline.start());
            for(unsigned i=0;i<8;++i) send(pipeline,i);
            wait_audio([&]{return pipeline.levels().pacing_waits>0;});
            assert(pipeline.levels().packets_decoded==5 && pipeline.levels().output_drops==0);
            if(mode=="overflow-live-edge") {
                for(unsigned i=8;i<21;++i) send(pipeline,i);
                assert(pipeline.levels().input_queue_drops>0);
                {std::lock_guard lock(mutex);release_output=true;release_budget=1;}
                wait_audio([&]{return pipeline.levels().packets_decoded==6;});
                assert(pipeline.levels().sequence_reanchors>0 && pipeline.levels().conceal_samples==0);
                {std::lock_guard lock(mutex);assert(decoded_sequences.back()==9);}
            } else {
                {std::lock_guard lock(mutex);release_output=true;}
                wait_audio([&]{return pipeline.levels().packets_decoded==8;});
            }
            assert(pipeline.levels().output_drops==0);pipeline.stop();return 0;
        }
        if(mode=="gap-live-edge") {
            AudioPipeline pipeline;pipeline.configure(100,30);assert(pipeline.start());
            for(unsigned i=0;i<3;++i) send(pipeline,i);
            await([]{return starts==1;});
            for(unsigned i=20;i<23;++i) send(pipeline,i);
            {std::lock_guard lock(mutex);release_output=true;release_budget=2;}
            wait_audio([&]{return pipeline.levels().sequence_reanchors>0 && pipeline.levels().packets_decoded>=4;});
            assert(pipeline.levels().conceal_samples==1440 && pipeline.levels().output_drops==0);
            {std::lock_guard lock(mutex);assert(decoded_sequences.back()==20 && decoder_generation==1);}
            pipeline.stop();return 0;
        }
        if(mode=="duration-20ms" || mode=="duration-2.5ms" || mode=="duration-120ms") {
            const bool small=mode=="duration-2.5ms";
            const bool large=mode=="duration-120ms";
            AudioPipeline pipeline;pipeline.configure(100,small?100:60);assert(pipeline.start());
            const unsigned count=small?40:large?1:3,samples=small?120:large?5760:960;
            for(unsigned i=0;i<count;++i) send(pipeline,large?2:i,small?0x80:large?0x1b:0xf8,samples);
            await([]{return starts==1;});
            wait_audio([&]{return pipeline.levels().packets_decoded==count;});
            assert(pipeline.levels().samples_decoded==count*samples);
            assert(pipeline.levels().packet_duration_us==(small?2500u:large?120000u:20000u));
            assert(pipeline.levels().output_drops==0);pipeline.stop();return 0;
        }
        if(mode=="adaptive") {
            AudioPipeline pipeline;pipeline.configure(100,40,true);assert(pipeline.start());
            unsigned sequence=0;
            for(const auto target:{40,60,80,100}) {
                {std::lock_guard lock(mutex);release_output=false;}
                for(int i=0;i<target/10;++i) send(pipeline,sequence++);
                wait_audio([&]{return pipeline.levels().packets_decoded==sequence;});
                assert(pipeline.levels().target_buffer_ms==static_cast<unsigned>(target));
                {std::lock_guard lock(mutex);release_output=true;}
                wait_audio([&]{return pipeline.levels().samples_played==sequence*480 && pipeline.levels().buffered_ms==0;});
                wait_audio([&]{return pipeline.levels().target_buffer_ms==static_cast<unsigned>(std::min(100,target+20));});
            }
            {std::lock_guard lock(mutex);assert(starts==1 && stops==0 && decoder_generation==1);}
            pipeline.stop();return 0;
        }
        if (std::string(argv[1]) == "reprime") {
            AudioPipeline pipeline;pipeline.configure(100,40);assert(pipeline.start());
            const auto send=[&](uint16_t sequence) {
                uint8_t payload[]={0xf0,static_cast<uint8_t>(sequence)};
                PeerAudioPacket packet{};packet.data=payload;packet.size=sizeof(payload);
                packet.ssrc=1;packet.sequence=sequence;packet.timestamp=480*sequence;packet.payload_type=111;
                pipeline.submit(packet);
            };
            for(unsigned i=0;i<4;++i) send(i);
            await([] {return starts==1;});
            {std::lock_guard lock(mutex);release_output=true;}
            wait_audio([&]{return pipeline.levels().samples_played==1920 && pipeline.levels().underruns==1;});
            {std::lock_guard lock(mutex);release_output=false;}
            send(4);std::this_thread::sleep_for(std::chrono::milliseconds(40));
            assert(pipeline.levels().packets_decoded==4); // A single packet cannot restart jitter churn.
            for(unsigned i=5;i<8;++i) send(i);
            wait_audio([&]{return pipeline.levels().packets_decoded==8;});
            {std::lock_guard lock(mutex);
             assert(starts==1 && stops==0 && decoder_generation==1);
             assert(decoded_sequences==std::vector<int>({0,1,2,3,4,5,6,7}));}
            pipeline.stop();return 0;
        }
        if (std::string(argv[1]) == "timeline")
            check_output_timeline();
        else {
            assert(std::string(argv[1]) == "ssrc");
            check_ssrc_timeline();
        }
        return 0;
    }
    AudioPipeline pipeline;
    pipeline.configure(1000, 30);
    assert(pipeline.start());
    submit_epoch(pipeline, 1);
    await([] { return starts == 1; });
    {
        std::lock_guard lock(mutex);
        assert(decode_generations == std::vector<int>({1, 1, 1}));
        assert(decoded_sequences == std::vector<int>({0, 1, 2}));
        assert(std::all_of(reclaim_queries_at_append.begin(),reclaim_queries_at_append.end(),[](int count){return count>=1;}));
        release_output = true;
    }
    wait_audio([&] { return pipeline.levels().samples_played==1440; });
    {
        std::lock_guard lock(mutex);
        assert(output.empty());
        release_output = false;
    }

    submit_epoch(pipeline, 2);
    wait_audio([&] { return pipeline.levels().packets_decoded==6; });
    {
        std::lock_guard lock(mutex);
        assert(decode_generations == std::vector<int>({1, 1, 1, 2, 2, 2}));
        assert(decoded_sequences == std::vector<int>({0, 1, 2, 0, 1, 2}));
        assert(output.size() == 3);
        assert(starts==1 && stops==0); // No Stop/Start on a transient underrun.
    }
    pipeline.stop();
    return 0;
}
