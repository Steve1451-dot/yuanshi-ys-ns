#include "stream/audio/AudioDiagnostics.hpp"
#include <cassert>
#include <map>
#include <string>

int main() {
    AudioLevels levels;
    levels.samples_decoded=48000;levels.conceal_samples=1440;levels.sequence_reanchors=1;
    levels.input_queue_drops=7;levels.pacing_waits=10;levels.target_buffer_ms=80;levels.buffered_ms=60;
    levels.receive_gap_max_us=20000;levels.worker_gap_max_us=5000;levels.decode_max_us=300;
    levels.packet_duration_us=10000;
    std::map<std::string,int> events;
    opennow::audio::ReportOutputHealth(levels,[&](const char* tag,int value,long http){
        assert(http==0);events[tag]=value;
    });
    assert(events["AUDIO_SAMPLES_DECODED"]==48000 && events["AUDIO_CONCEAL_SAMPLES"]==1440);
    assert(events["AUDIO_SEQUENCE_REANCHORS"]==1 && events["AUDIO_INPUT_QUEUE_DROPS"]==7);
    assert(events["AUDIO_OUTPUT_DROPS"]==0 && events["AUDIO_PACING_WAITS"]==10);
    assert(events["AUDIO_BUFFERED_MS"]==60 && events["AUDIO_TARGET_BUFFER_MS"]==80);
    assert(events["AUDIO_PACKET_DURATION_US"]==10000 && events["AUDIO_DECODE_MAX_US"]==300);
    assert(events["AUDIO_RECEIVE_GAP_MAX_US"]==20000 && events["AUDIO_WORKER_GAP_MAX_US"]==5000);
    assert(events["AUDIO_OUTPUT_READY"]==0 && events["AUDIO_OUTPUT_STARTED"]==0); // Counts aren't playback proof.
}
