#include "AudioPipeline.hpp"
#include "AudioLatencyPolicy.hpp"
#include "AudioJitterPolicy.hpp"
#include "AudioPlaybackTimeline.hpp"
#include "AudioRtpUtils.hpp"
#include "../../stream_diagnostics.hpp"
#include "app_paths.hpp"
#include "stream/BoundedDiagnosticWriter.hpp"

#include "peer_connection.h"
#include <switch.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <deque>
#include <fstream>
#include <malloc.h>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kFrameSamples = 480; // NVIDIA advertises ptime=10 ms.
constexpr int kMaxPackets = 64; // Bounded below by duration as well as count.
constexpr unsigned kInputQueueSamples = 5760; // 120 ms, not 64 long packets.
constexpr int kOutputBufferCount = 64; // Also fits a 100 ms reserve of 2.5 ms Opus.
constexpr size_t kOutputBufferBytes = 0x4000;
constexpr int kFadeSilenceFrames = 2400; // 50 ms
constexpr int kFadeTotalFrames = 12000;  // 250 ms

uint64_t monotonic_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void write_be32(uint8_t* p, uint32_t value) {
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
}

}

struct AudioPipeline::Impl {
    struct Packet {
        std::vector<uint8_t> payload;
        uint32_t timestamp = 0;
        uint32_t ssrc = 0;
        uint16_t sequence = 0;
        uint8_t payload_type = 0;
        uint64_t arrival_us = 0;
        unsigned samples = kFrameSamples;
    };

    std::atomic<bool> running {false};
    std::atomic<bool> reset_epoch_requested {false};
    std::atomic<uint64_t> last_receive_us {0};
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<Packet> packets;
    uint16_t expected_sequence = 0;
    bool have_expected = false;
    bool primed = false;
    int volume_percent = 1200;
    float applied_gain = 1.0f;
    std::atomic<int> last_input_peak {0};
    std::atomic<int> last_input_rms {0};
    std::atomic<int> last_output_peak {0};
    std::atomic<int> last_output_rms {0};
    std::atomic<int> last_applied_gain_x100 {100};
    std::atomic<uint64_t> limited_frames {0};
    unsigned prime_samples = 1920,output_start_samples = 1920;
    unsigned prefill_samples_remaining = 0; // Worker only: finish a recovery batch before re-arming.
    std::atomic<int> target_buffer_ms {40};
    bool adaptive_recovery = false;
    unsigned queued_input_samples = 0,queued_output_samples = 0;
    std::atomic<unsigned> output_buffered_samples {0},packet_duration_us {0};
    bool input_overflowed = false;
    uint32_t expected_timestamp = 0;
    unsigned previous_packet_samples = kFrameSamples;
    opennow::audio::GapConcealBudget gap_budget;
    uint64_t initial_jitter_hold_us = 40000;
    uint64_t resync_jitter_hold_us = 30000;
    uint64_t jitter_hold_until_us = 0;
    uint64_t missing_packet_since_us = 0;
    int fade_frames_processed = 0;

    HwopusDecoder decoder {};
    bool decoder_ready = false;
    struct OutputSlot {
        AudioOutBuffer buffer {};
        uint8_t* storage = nullptr;
        bool free = true;
    };
    std::array<OutputSlot, kOutputBufferCount> output_slots {};
    std::atomic<bool> output_ready {false};
    std::atomic<bool> output_started {false};
    std::atomic<uint32_t> last_output_error {0};
    std::atomic<uint64_t> output_errors {0};
    size_t queued_output_buffers = 0;
    bool output_starved = false;
    uint64_t played_sample_base = 0;
    std::atomic<uint64_t> submitted_samples {0};
    std::atomic<uint64_t> played_samples {0};
    std::atomic<uint64_t> played_samples_observed_at_us {0};
    std::atomic<uint32_t> audio_ssrc {0};
    mutable std::mutex sender_report_mutex;
    opennow::audio::PlaybackTimeline<kOutputBufferCount + 1> playback_timeline;
    uint64_t sr_ntp_us = 0;
    uint32_t sr_rtp_timestamp = 0;
    std::atomic<bool> have_sender_report {false};

    std::atomic<uint64_t> packets_rx {0};
    std::atomic<uint64_t> payload_bytes_rx {0};
    std::atomic<uint64_t> packets_decoded {0};
    std::atomic<uint64_t> decode_attempts {0};
    std::atomic<uint64_t> samples_decoded {0};
    std::atomic<uint64_t> sequence_gaps {0};
    std::atomic<uint64_t> late_drops {0};
    std::atomic<uint64_t> queue_drops {0};
    std::atomic<uint64_t> output_throttle_drops {0};
    std::atomic<uint64_t> decode_errors {0};
    std::atomic<uint64_t> plc_frames {0};
    std::atomic<uint64_t> underruns {0};
    std::atomic<uint64_t> red_packets {0};
    std::atomic<uint64_t> red_recoveries {0};
    std::atomic<uint64_t> epoch_resets {0};
    std::atomic<uint64_t> input_queue_drops {0},conceal_samples {0},sequence_reanchors {0},pacing_waits {0};
    std::atomic<uint64_t> receive_gap_max_us {0},worker_gap_max_us {0},decode_max_us {0};
    uint64_t session_started_us = 0;
    uint64_t last_wait_log_us = 0;
    opennow::stream::BoundedDiagnosticWriter diagnostic_writer;
    opennow::stream::WorkerScheduling scheduling;

    void log(const std::string& message) {
        if (!opennow::StreamDiagnosticsEnabled())
            return;
        diagnostic_writer.submit(std::to_string(monotonic_us())+" "+message+'\n');
    }

    bool initialize_audio() {
        Result rc = hwopusDecoderInitialize(&decoder, kSampleRate, kChannels);
        if (R_FAILED(rc)) {
            record_output_error(rc);
            log("INIT hwopus_failed rc=0x" + hex_result(rc));
            return false;
        }
        decoder_ready = true;

        rc = audoutInitialize();
        if (R_FAILED(rc)) {
            record_output_error(rc);
            log("INIT audout_failed rc=0x" + hex_result(rc));
            return false;
        }
        if (audoutGetSampleRate() != kSampleRate || audoutGetChannelCount() != kChannels ||
            audoutGetPcmFormat() != PcmFormat_Int16) {
            log("INIT audout_format_unsupported rate=" + std::to_string(audoutGetSampleRate()) +
                " channels=" + std::to_string(audoutGetChannelCount()) +
                " format=" + std::to_string(static_cast<int>(audoutGetPcmFormat())));
            audoutExit();
            return false;
        }
        // Per-stream volume only: never change the console's master volume.
        // Hardware and emulators must not depend on an inherited sink volume.
        record_output_error(audoutSetAudioOutVolume(1.0f));
        for (auto& slot : output_slots) {
            slot.storage = static_cast<uint8_t*>(memalign(0x1000, kOutputBufferBytes));
            if (!slot.storage) {
                log("INIT audout_buffer_allocation_failed");
                for (auto& allocated : output_slots) {
                    free(allocated.storage);
                    allocated.storage = nullptr;
                }
                audoutExit();
                return false;
            }
            std::memset(slot.storage, 0, kOutputBufferBytes);
            slot.buffer.next = nullptr;
            slot.buffer.buffer = slot.storage;
            slot.buffer.buffer_size = kOutputBufferBytes;
            slot.buffer.data_size = 0;
            slot.buffer.data_offset = 0;
            slot.free = true;
        }
        u64 raw_played = 0;
        if (R_SUCCEEDED(audoutGetAudioOutPlayedSampleCount(&raw_played)))
            played_sample_base = raw_played;
        output_ready.store(true);
        log("INIT ok backend=audout rate=48000 channels=2 prebufferSamples=" +
            std::to_string(prime_samples) + " buffers=64 outputLimitSamples=" +
            std::to_string(opennow::audio::AudioQueueHighSamples(target_buffer_ms.load())) + " startupHoldMs=" +
            std::to_string(initial_jitter_hold_us / 1000) + " gain=" +
            std::to_string(volume_percent));
        return true;
    }

    static std::string hex_result(Result rc) {
        char text[16];
        std::snprintf(text, sizeof(text), "%08X", static_cast<unsigned int>(rc));
        return text;
    }

    void record_output_error(Result rc) {
        if (R_FAILED(rc)) {
            last_output_error.store(rc);
            output_errors++;
        }
    }

    void shutdown_audio() {
        if (output_ready.exchange(false)) {
            // Stop/close before freeing ANY PCM pages, including buffers that
            // were queued but never started. A failed flush is not ownership.
            if (output_started || queued_output_buffers > 0)
                record_output_error(audoutStopAudioOut());
            output_started = false;
            bool flushed = false;
            record_output_error(audoutFlushAudioOutBuffers(&flushed));
            audoutExit();
            for (auto& slot : output_slots) {
                free(slot.storage);
                slot.storage = nullptr;
                slot.free = true;
            }
            queued_output_buffers = 0;
            queued_output_samples = 0;output_buffered_samples.store(0);
        }
        if (decoder_ready) {
            hwopusDecoderExit(&decoder);
            decoder_ready = false;
        }
    }

    void refresh_played_samples() {
        u64 raw_played = 0;
        if (R_FAILED(audoutGetAudioOutPlayedSampleCount(&raw_played)))
            return;
        const uint64_t relative = raw_played >= played_sample_base ? raw_played - played_sample_base : raw_played;
        std::lock_guard<std::mutex> lock(sender_report_mutex);
        played_samples.store(std::min(relative, submitted_samples.load()),
                             std::memory_order_relaxed);
        played_samples_observed_at_us.store(monotonic_us(), std::memory_order_release);
        playback_timeline.discardBefore(played_samples.load(std::memory_order_relaxed));
    }

    void reclaim_output_buffers() {
        AudioOutBuffer* released = nullptr;
        u32 released_count = 0;
        while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&released, &released_count)) &&
               released && released_count > 0) {
            for (auto& slot : output_slots) {
                if (&slot.buffer == released) {
                    if (!slot.free && queued_output_buffers > 0) {
                        --queued_output_buffers;
                        const auto samples=static_cast<unsigned>(slot.buffer.data_size/(kChannels*sizeof(int16_t)));
                        queued_output_samples-=std::min(queued_output_samples,samples);
                        output_buffered_samples.store(queued_output_samples);
                    }
                    slot.free = true;
                    break;
                }
            }
            released = nullptr;
            released_count = 0;
        }
        refresh_played_samples();
        if (output_started && queued_output_buffers == 0 && !output_starved && prefill_samples_remaining==0) {
            // A temporary network underrun is not a session stop. Keep the
            // sink running so new PCM resumes without Stop/Start churn (and
            // without restarting the device's played-sample clock).
            output_starved = true;
            underruns++;
            // Rebuild the configured small reserve after a real underrun.
            // Starting immediately with one packet repeatedly starves on a
            // jittery link. Keep the output clock, decoder and queued input.
            {std::lock_guard lock(mutex);
             const int target=opennow::audio::RecoverTargetMs(target_buffer_ms.load(),adaptive_recovery);
             target_buffer_ms.store(target);prime_samples=static_cast<unsigned>(target)*48;
             primed=false;missing_packet_since_us=0;
             jitter_hold_until_us=monotonic_us()+opennow::audio::ResyncJitterHoldUs(target);}
            log("AUDOUT underrun; re-priming while output stays running");
        }
    }

    OutputSlot* acquire_output_slot() {
        for (auto& slot : output_slots)
            if (slot.free)
                return &slot;
        return nullptr;
    }

    bool ensure_output_started() {
        if (output_started || queued_output_samples < output_start_samples)
            return true;
        const Result rc = audoutStartAudioOut();
        if (R_FAILED(rc)) {
            record_output_error(rc);
            log("AUDOUT start_failed rc=0x" + hex_result(rc));
            return false;
        }
        output_started = true;
        log("AUDOUT started queued=" + std::to_string(queued_output_buffers));
        return true;
    }

    void submit_pcm(const int16_t* pcm, int samples, uint32_t timestamp, uint32_t ssrc) {
        if (!output_ready.load() || !pcm || samples <= 0)
            return;
        reclaim_output_buffers();
        if (queued_output_buffers >= output_slots.size()) {
            const uint64_t drops = output_throttle_drops.fetch_add(1) + 1;
            queue_drops++;
            if (drops == 1 || drops % 50 == 0)
                log("AUDOUT live_edge_drop queued=" + std::to_string(queued_output_buffers) +
                    " limit=" + std::to_string(output_slots.size()) +
                    " count=" + std::to_string(drops));
            return;
        }
        const auto* source = reinterpret_cast<const uint8_t*>(pcm);
        size_t remaining = static_cast<size_t>(samples) * kChannels * sizeof(int16_t);
        while (remaining > 0 && running.load()) {
            {
                std::lock_guard<std::mutex> lock(sender_report_mutex);
                if (playback_timeline.full()) {
                    queue_drops++;
                    return;
                }
            }
            OutputSlot* slot = acquire_output_slot();
            if (!slot) {
                queue_drops++;
                log("AUDOUT no_free_buffer queued=" + std::to_string(queued_output_buffers));
                return;
            }
            const size_t copy_size = std::min(remaining, kOutputBufferBytes);
            std::memcpy(slot->storage, source, copy_size);
            armDCacheFlush(slot->storage, copy_size);
            slot->buffer.next = nullptr;
            slot->buffer.data_offset = 0;
            slot->buffer.data_size = copy_size;
            if (decode_attempts.load() <= 12)
                log("AUDOUT enqueue_begin bytes=" + std::to_string(copy_size));
            Result rc = audoutAppendAudioOutBuffer(&slot->buffer);
            if (R_FAILED(rc) && !output_started) {
                const Result start_rc = audoutStartAudioOut();
                if (R_SUCCEEDED(start_rc)) {
                    output_started = true;
                    rc = audoutAppendAudioOutBuffer(&slot->buffer);
                }
            }
            if (R_FAILED(rc)) {
                record_output_error(rc);
                queue_drops++;
                log("AUDOUT enqueue_failed rc=0x" + hex_result(rc));
                bool contains = false;
                // An IPC failure cannot prove that the service no longer owns
                // these pages. Keep them reserved until output teardown.
                slot->free = R_SUCCEEDED(audoutContainsAudioOutBuffer(&slot->buffer, &contains)) && !contains;
                return;
            }
            slot->free = false;
            output_starved = false;
            ++queued_output_buffers;
            const uint64_t frame_count = copy_size / (kChannels * sizeof(int16_t));
            queued_output_samples+=static_cast<unsigned>(frame_count);
            prefill_samples_remaining-=std::min(prefill_samples_remaining,static_cast<unsigned>(frame_count));
            output_buffered_samples.store(queued_output_samples);
            {
                std::lock_guard<std::mutex> lock(sender_report_mutex);
                playback_timeline.append(submitted_samples.load(), timestamp, ssrc);
                submitted_samples.fetch_add(frame_count);
            }
            timestamp += static_cast<uint32_t>(frame_count);
            if (decode_attempts.load() <= 12)
                log("AUDOUT enqueue_ok frames=" + std::to_string(frame_count) +
                    " queued=" + std::to_string(queued_output_buffers));
            if (!ensure_output_started())
                return;
            source += copy_size;
            remaining -= copy_size;
        }
        refresh_played_samples();
    }

    void submit_plc(const Packet& packet) {
        std::array<int16_t, 5760 * kChannels> silence {};
        const auto samples=std::clamp(packet.samples,120u,5760u);
        submit_pcm(silence.data(), static_cast<int>(samples), packet.timestamp, packet.ssrc);
        plc_frames++;
        conceal_samples.fetch_add(samples);
    }

    void apply_volume_boost(int16_t* pcm, int sample_count, uint64_t attempt) {
        if (!pcm || sample_count <= 0)
            return;

        int input_peak = 0;
        uint64_t input_square_sum = 0;
        for (int i = 0; i < sample_count; ++i) {
            const int sample = static_cast<int>(pcm[i]);
            input_peak = std::max(input_peak, std::abs(sample));
            input_square_sum += static_cast<uint64_t>(static_cast<int64_t>(sample) * sample);
        }

        const float requested_gain = static_cast<float>(volume_percent) / 100.0f;
        float safe_gain = requested_gain;
        if (input_peak > 0 && requested_gain > 1.0f)
            safe_gain = std::min(requested_gain, 30000.0f / static_cast<float>(input_peak));

        // Fast attack prevents clipping; slower release avoids audible pumping.
        if (safe_gain < applied_gain)
            applied_gain = safe_gain;
        else
            applied_gain += (safe_gain - applied_gain) * 0.08f;
        applied_gain = std::max(0.0f, std::min(requested_gain, applied_gain));
        if (safe_gain + 0.01f < requested_gain)
            limited_frames++;

        int output_peak = 0;
        uint64_t output_square_sum = 0;
        for (int i = 0; i < sample_count; ++i) {
            float fade = 1.0f;
            if (fade_frames_processed < kFadeTotalFrames) {
                const int frame = fade_frames_processed + i / kChannels;
                if (frame < kFadeSilenceFrames) {
                    fade = 0.0f;
                } else {
                    fade = std::min(1.0f, static_cast<float>(frame - kFadeSilenceFrames) /
                                              static_cast<float>(kFadeTotalFrames - kFadeSilenceFrames));
                }
            }
            int32_t amplified = static_cast<int32_t>(
                std::lround(static_cast<float>(pcm[i]) * applied_gain * fade));
            amplified = std::max<int32_t>(-32768, std::min<int32_t>(32767, amplified));
            pcm[i] = static_cast<int16_t>(amplified);
            output_peak = std::max(output_peak, std::abs(static_cast<int>(pcm[i])));
            output_square_sum += static_cast<uint64_t>(static_cast<int64_t>(pcm[i]) * pcm[i]);
        }
        fade_frames_processed = std::min(kFadeTotalFrames,
                                         fade_frames_processed + sample_count / kChannels);

        const int input_rms = static_cast<int>(std::sqrt(
            static_cast<double>(input_square_sum) / static_cast<double>(sample_count)));
        const int output_rms = static_cast<int>(std::sqrt(
            static_cast<double>(output_square_sum) / static_cast<double>(sample_count)));
        last_input_peak.store(input_peak);
        last_input_rms.store(input_rms);
        last_output_peak.store(output_peak);
        last_output_rms.store(output_rms);
        last_applied_gain_x100.store(static_cast<int>(std::lround(applied_gain * 100.0f)));

        if (attempt == 1 || attempt % 500 == 0) {
            log("LEVEL inputPeak=" + std::to_string(input_peak) +
                " inputRms=" + std::to_string(input_rms) +
                " outputPeak=" + std::to_string(output_peak) +
                " outputRms=" + std::to_string(output_rms) +
                " requestedGainX100=" + std::to_string(volume_percent) +
                " appliedGainX100=" + std::to_string(last_applied_gain_x100.load()) +
                " limitedFrames=" + std::to_string(limited_frames.load()));
        }
    }

    void reset_media_epoch(const char* reason) {
        if (output_ready.load()) {
            // Do not re-arm buffers still owned by audout. They are reclaimed
            // normally, keeping the restart click-free and avoiding 0x1099.
            reclaim_output_buffers();
        }
        if (decoder_ready) {
            hwopusDecoderExit(&decoder);
            decoder_ready = R_SUCCEEDED(hwopusDecoderInitialize(&decoder, kSampleRate, kChannels));
        }
        applied_gain = 0.0f;
        fade_frames_processed = 0;
        epoch_resets++;
        gap_budget.reset();previous_packet_samples=kFrameSamples;
        log("EPOCH reset reason=" + std::string(reason ? reason : "transport_gap") +
            " count=" + std::to_string(epoch_resets.load()) +
            " audoutQueued=" + std::to_string(queued_output_buffers));
    }

    void decode_packet(const Packet& packet) {
        const uint64_t attempt = decode_attempts.fetch_add(1) + 1;
        if (attempt <= 12)
            log("DECODE begin attempt=" + std::to_string(attempt) +
                " seq=" + std::to_string(packet.sequence) +
                " pt=" + std::to_string(packet.payload_type) +
                " bytes=" + std::to_string(packet.payload.size()) +
                " rtp=" + std::to_string(packet.timestamp));
        if (!decoder_ready) {
            decode_errors++;
            submit_plc(packet);
            return;
        }
        opennow::audio::ParsedPayload payload =
            opennow::audio::ParseRedPrimary(packet.payload.data(), packet.payload.size(), packet.payload_type);
        if (!payload.data || payload.size == 0) {
            decode_errors++;
            log("DECODE malformed_red seq=" + std::to_string(packet.sequence));
            submit_plc(packet);
            return;
        }
        if (payload.red)
            red_packets++;

        std::vector<uint8_t> input(sizeof(HwopusHeader) + payload.size);
        write_be32(input.data(), static_cast<uint32_t>(payload.size));
        write_be32(input.data() + 4, 0);
        std::memcpy(input.data() + sizeof(HwopusHeader), payload.data, payload.size);
        std::array<int16_t, 5760 * kChannels> pcm {};
        int32_t decoded_bytes = 0;
        int32_t decoded_samples = 0;
        const auto decode_started=monotonic_us();
        const Result rc = hwopusDecodeInterleaved(&decoder, &decoded_bytes, &decoded_samples,
                                                   input.data(), input.size(), pcm.data(),
                                                   pcm.size() * sizeof(int16_t));
        decode_max_us.store(std::max(decode_max_us.load(),monotonic_us()-decode_started));
        if (attempt <= 12)
            log("DECODE hwopus_return attempt=" + std::to_string(attempt) +
                " rc=0x" + hex_result(rc) +
                " samples=" + std::to_string(decoded_samples) +
                " consumed=" + std::to_string(decoded_bytes));
        if (R_FAILED(rc) || decoded_samples <= 0 || decoded_samples>5760) {
            decode_errors++;
            log("DECODE failed seq=" + std::to_string(packet.sequence) +
                " pt=" + std::to_string(packet.payload_type) +
                " bytes=" + std::to_string(payload.size) + " rc=0x" + hex_result(rc));
            submit_plc(packet);
            return;
        }
        apply_volume_boost(pcm.data(), decoded_samples * kChannels, attempt);
        submit_pcm(pcm.data(), decoded_samples, packet.timestamp, packet.ssrc);
        if (attempt <= 12)
            log("DECODE submit_done attempt=" + std::to_string(attempt) +
                " submitted=" + std::to_string(submitted_samples.load()));
        packets_decoded++;
        samples_decoded += decoded_samples;
        if (packets_decoded.load() == 1 || packets_decoded.load() % 500 == 0) {
            size_t queue_size = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                queue_size = packets.size();
            }
            log("PROGRESS packets=" + std::to_string(packets_decoded.load()) +
                " samples=" + std::to_string(samples_decoded.load()) +
                " queue=" + std::to_string(queue_size) +
                " played=" + std::to_string(played_samples.load()));
        }
    }

    bool take_next(Packet& packet) {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            if (!running.load() || reset_epoch_requested.load())
                return false;

            const uint64_t now_us = monotonic_us();
            const bool hold_finished = jitter_hold_until_us == 0 || now_us >= jitter_hold_until_us;
            if (packets.empty() || !hold_finished ||
                (!primed && queued_input_samples < prime_samples)) {
                uint64_t wait_us = 12000;
                if (!hold_finished)
                    wait_us = std::min(wait_us, jitter_hold_until_us - now_us);
                if (cv.wait_for(lock, std::chrono::microseconds(wait_us)) ==
                    std::cv_status::timeout)
                    return false;
                continue;
            }

            if (!primed) {
                primed = true;
                expected_sequence = packets.front().sequence;
                expected_timestamp=packets.front().timestamp;
                previous_packet_samples=packets.front().samples;
                // The running sink may consume the first recovery block while
                // the rest is still decoding. Do not cancel this same refill
                // and demand another full reserve after each individual block.
                prefill_samples_remaining=prime_samples;
                gap_budget.reset();input_overflowed=false;
                have_expected = true;
                jitter_hold_until_us = 0;
                missing_packet_since_us = 0;
                log("JITTER primed packets=" + std::to_string(packets.size()) +
                    " seq=" + std::to_string(expected_sequence));
            }

            while (!packets.empty() &&
                   static_cast<int16_t>(packets.front().sequence - expected_sequence) < 0) {
                queued_input_samples-=packets.front().samples;
                packets.pop_front();
                late_drops++;
            }
            if (packets.empty()) {
                missing_packet_since_us = 0;
                continue;
            }
            if(input_overflowed && static_cast<int16_t>(packets.front().sequence-expected_sequence)>0) {
                // Input deliberately evicted for bounded latency is not a
                // network hole to recreate with a train of silent packets.
                expected_sequence=packets.front().sequence;
                expected_timestamp=packets.front().timestamp;
                gap_budget.reset();sequence_reanchors++;
            }
            input_overflowed=false;

            auto found = std::find_if(packets.begin(), packets.end(), [this](const Packet& item) {
                return item.sequence == expected_sequence;
            });
            if (found != packets.end()) {
                queued_input_samples-=found->samples;
                packet = std::move(*found);
                packets.erase(found);
                expected_sequence++;
                expected_timestamp=packet.timestamp+packet.samples;
                previous_packet_samples=packet.samples;gap_budget.reset();
                missing_packet_since_us = 0;
                return true;
            }

            auto next = std::find_if(packets.begin(), packets.end(), [this](const Packet& item) {
                return item.sequence == static_cast<uint16_t>(expected_sequence + 1) &&
                       item.payload_type == 63;
            });
            if (next != packets.end()) {
                const auto redundant = opennow::audio::ParseFirstRedundant(
                    next->payload.data(), next->payload.size());
                if (redundant.data && redundant.size > 0) {
                    packet.payload.assign(redundant.data, redundant.data + redundant.size);
                    packet.timestamp = next->timestamp - redundant.timestamp_offset;
                    packet.ssrc = next->ssrc;
                    packet.sequence = expected_sequence;
                    packet.payload_type = 111;
                    packet.arrival_us = next->arrival_us;
                    packet.samples=opennow::audio::OpusPacketSamples48k(redundant.data,redundant.size);
                    if(!packet.samples) packet.samples=previous_packet_samples;
                    expected_sequence++;
                    expected_timestamp=packet.timestamp+packet.samples;
                    previous_packet_samples=packet.samples;gap_budget.reset();
                    missing_packet_since_us = 0;
                    red_recoveries++;
                    return true;
                }
            }

            if (missing_packet_since_us == 0)
                missing_packet_since_us = now_us;
            const uint64_t missing_us = now_us - missing_packet_since_us;
            const auto submitted=submitted_samples.load();
            const auto played=opennow::audio::EstimatePlayedSamples(played_samples.load(),
                played_samples_observed_at_us.load(),now_us,submitted,kSampleRate);
            const uint64_t buffered_us=(submitted-played)*1000000/kSampleRate;
            const uint64_t grace_us = opennow::audio::PacketReorderGraceUs(buffered_us);
            if (missing_us < grace_us) {
                cv.wait_for(lock, std::chrono::microseconds(grace_us - missing_us));
                continue;
            }

            if(!gap_budget.allow(previous_packet_samples)) {
                expected_sequence=packets.front().sequence;
                expected_timestamp=packets.front().timestamp;
                gap_budget.reset();sequence_reanchors++;missing_packet_since_us=0;
                continue; // Resume real audio, without resetting Opus or the output device.
            }
            sequence_gaps++;
            packet.ssrc = packets.front().ssrc;
            packet.timestamp = expected_timestamp;
            packet.samples=previous_packet_samples;
            expected_timestamp+=packet.samples;
            expected_sequence++;
            missing_packet_since_us = 0;
            return true; // Empty payload means PLC.
        }
    }

    void run() {
        scheduling.apply(opennow::stream::WorkerRole::Audio);
        session_started_us = monotonic_us();
        last_wait_log_us = session_started_us;
        if (!initialize_audio()) {
            running.store(false);
            shutdown_audio();
            return;
        }
        auto last_work_us=monotonic_us();
        while (running.load()) {
            const auto work_us=monotonic_us();
            worker_gap_max_us.store(std::max(worker_gap_max_us.load(),work_us-last_work_us));last_work_us=work_us;
            if (reset_epoch_requested.exchange(false))
                reset_media_epoch("transport_gap");
            if(queued_output_samples>=opennow::audio::AudioQueueHighSamples(target_buffer_ms.load()))
                reclaim_output_buffers();
            if(queued_output_samples>=opennow::audio::AudioQueueHighSamples(target_buffer_ms.load())) {
                // Decode only when the sink can use the result. Previously a
                // receive burst was decoded then thrown away at the PCM cap.
                pacing_waits++;
                std::unique_lock lock(mutex);
                cv.wait_for(lock,std::chrono::milliseconds(2),[this]{return !running.load() || reset_epoch_requested.load();});
                continue;
            }
            Packet packet;
            if (!take_next(packet)) {
                if (output_ready.load() && queued_output_buffers > 0)
                    reclaim_output_buffers();
                const uint64_t now_us = monotonic_us();
                if (packets_rx.load() == 0 && now_us - last_wait_log_us >= 5000000) {
                    log("WAIT no_audio_rtp elapsedMs=" +
                        std::to_string((now_us - session_started_us) / 1000));
                    last_wait_log_us = now_us;
                }
                continue;
            }
            if (packet.payload.empty())
                submit_plc(packet);
            else
                decode_packet(packet);
        }
        log("SUMMARY rx=" + std::to_string(packets_rx.load()) +
            " decoded=" + std::to_string(packets_decoded.load()) +
            " samples=" + std::to_string(samples_decoded.load()) +
            " gaps=" + std::to_string(sequence_gaps.load()) +
            " late=" + std::to_string(late_drops.load()) +
                " drops=" + std::to_string(queue_drops.load()) +
                " outputThrottle=" + std::to_string(output_throttle_drops.load()) +
            " errors=" + std::to_string(decode_errors.load()) +
            " plc=" + std::to_string(plc_frames.load()) +
            " underruns=" + std::to_string(underruns.load()) +
            " red/recovered=" + std::to_string(red_packets.load()) + "/" +
            std::to_string(red_recoveries.load()) +
            " epochResets=" + std::to_string(epoch_resets.load()) +
            " inputPeak/Rms=" + std::to_string(last_input_peak.load()) + "/" +
            std::to_string(last_input_rms.load()) +
            " outputPeak/Rms=" + std::to_string(last_output_peak.load()) + "/" +
            std::to_string(last_output_rms.load()) +
            " gainX100=" + std::to_string(last_applied_gain_x100.load()) +
            " limitedFrames=" + std::to_string(limited_frames.load()));
        shutdown_audio();
    }
};

AudioPipeline::AudioPipeline() : impl_(std::make_unique<Impl>()) {}
AudioPipeline::~AudioPipeline() { stop(); }

void AudioPipeline::configure(int volume_percent, int target_buffer_ms, bool adaptive_recovery) {
    impl_->volume_percent = std::max(800, std::min(1600, volume_percent));
    impl_->target_buffer_ms.store(opennow::audio::ClampTargetBufferMs(target_buffer_ms));
    impl_->prime_samples=impl_->output_start_samples=static_cast<unsigned>(impl_->target_buffer_ms.load())*48;
    impl_->adaptive_recovery=adaptive_recovery;
    impl_->initial_jitter_hold_us = opennow::audio::InitialJitterHoldUs(target_buffer_ms);
    impl_->resync_jitter_hold_us = opennow::audio::ResyncJitterHoldUs(target_buffer_ms);
}

bool AudioPipeline::start() {
    if (impl_->running.load())
        return true;
    if (impl_->worker.joinable())
        impl_->worker.join();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->packets.clear();
        impl_->queued_input_samples=0;impl_->input_overflowed=false;
        impl_->gap_budget.reset();impl_->expected_timestamp=0;impl_->previous_packet_samples=kFrameSamples;
        impl_->expected_sequence = 0;
        impl_->have_expected = false;
        impl_->primed = false;
        impl_->jitter_hold_until_us = 0;
        impl_->missing_packet_since_us = 0;
    }
    impl_->reset_epoch_requested.store(false);
    impl_->last_receive_us.store(0);
    impl_->submitted_samples.store(0);
    impl_->played_samples.store(0);
    impl_->played_samples_observed_at_us.store(0);
    impl_->played_sample_base = 0;
    impl_->output_starved = false;
    impl_->queued_output_samples=0;impl_->output_buffered_samples.store(0);impl_->packet_duration_us.store(0);
    impl_->prefill_samples_remaining=0;
    {
        std::lock_guard<std::mutex> lock(impl_->sender_report_mutex);
        impl_->playback_timeline.clear();
    }
    impl_->audio_ssrc.store(0);
    impl_->have_sender_report.store(false);
    impl_->packets_rx.store(0);
    impl_->payload_bytes_rx.store(0);
    impl_->packets_decoded.store(0);
    impl_->decode_attempts.store(0);
    impl_->samples_decoded.store(0);
    impl_->sequence_gaps.store(0);
    impl_->late_drops.store(0);
    impl_->queue_drops.store(0);
    impl_->output_throttle_drops.store(0);
    impl_->decode_errors.store(0);
    impl_->plc_frames.store(0);
    impl_->underruns.store(0);
    impl_->red_packets.store(0);
    impl_->red_recoveries.store(0);
    impl_->epoch_resets.store(0);
    impl_->input_queue_drops.store(0);impl_->conceal_samples.store(0);impl_->sequence_reanchors.store(0);impl_->pacing_waits.store(0);
    impl_->receive_gap_max_us.store(0);impl_->worker_gap_max_us.store(0);impl_->decode_max_us.store(0);
    impl_->applied_gain = 0.0f;
    impl_->fade_frames_processed = 0;
    impl_->last_input_peak.store(0);
    impl_->last_input_rms.store(0);
    impl_->last_output_peak.store(0);
    impl_->last_output_rms.store(0);
    impl_->last_applied_gain_x100.store(0);
    impl_->limited_frames.store(0);
    impl_->last_output_error.store(0);
    impl_->output_errors.store(0);
    impl_->running.store(true);
    if (opennow::StreamDiagnosticsEnabled()) {
        // Open/truncate/write only on the low-priority diagnostic owner. In
        // particular RTCP sender reports and jitter priming must never open an
        // SD file on the receiver or while holding the audio queue mutex.
        impl_->diagnostic_writer.start([path=opennow::AppHomePath()+"/audio.log",first=true](const std::string& batch) mutable {
            std::ofstream file(path,std::ios::binary|(first ? std::ios::trunc : std::ios::app));
            if(!file) return false;
            first=false;file<<batch;file.flush();return file.good();
        });
    }
    impl_->log("SESSION start");
    impl_->worker = std::thread(&Impl::run, impl_.get());
    return true;
}

void AudioPipeline::stop() {
    if (!impl_ || !impl_->running.exchange(false)) {
        if (impl_ && impl_->worker.joinable())
            impl_->worker.join();
        if(impl_) impl_->diagnostic_writer.stop();
        return;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable())
        impl_->worker.join();
    impl_->diagnostic_writer.stop();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->packets.clear();
}

void AudioPipeline::submit(const PeerAudioPacket& source) {
    if (!impl_->running.load() || !source.data || source.size == 0)
        return;
    const uint32_t previous_ssrc = impl_->audio_ssrc.exchange(source.ssrc);
    const bool ssrc_changed = previous_ssrc != 0 && previous_ssrc != source.ssrc;
    if (ssrc_changed) {
        std::lock_guard<std::mutex> lock(impl_->sender_report_mutex);
        impl_->have_sender_report.store(false, std::memory_order_release);
    }
    const uint64_t now_us = monotonic_us();
    const uint64_t previous_receive_us = impl_->last_receive_us.exchange(now_us);
    if(previous_receive_us && now_us>previous_receive_us)
        impl_->receive_gap_max_us.store(std::max(impl_->receive_gap_max_us.load(),now_us-previous_receive_us));
    Impl::Packet packet;
    packet.payload.assign(source.data, source.data + source.size);
    packet.timestamp = source.timestamp;
    packet.ssrc = source.ssrc;
    packet.sequence = source.sequence;
    packet.payload_type = source.payload_type;
    packet.arrival_us = now_us;
    const auto payload=opennow::audio::ParseRedPrimary(source.data,source.size,source.payload_type);
    packet.samples=opennow::audio::OpusPacketSamples48k(payload.data,payload.size);
    if(!packet.samples) packet.samples=kFrameSamples;
    impl_->packet_duration_us.store(static_cast<unsigned>(packet.samples*1000000ULL/kSampleRate));
    impl_->packets_rx++;
    impl_->payload_bytes_rx.fetch_add(source.size);

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (ssrc_changed) {
        impl_->packets.clear();
        impl_->queued_input_samples=0;impl_->input_overflowed=false;
        impl_->primed = false;
        impl_->have_expected = false;
        impl_->missing_packet_since_us = 0;
        impl_->jitter_hold_until_us = now_us + impl_->resync_jitter_hold_us;
        impl_->reset_epoch_requested.store(true);
    }
    if (impl_->jitter_hold_until_us == 0 && !impl_->primed)
        impl_->jitter_hold_until_us = now_us + impl_->initial_jitter_hold_us;
    if (opennow::audio::ShouldResetEpoch(previous_receive_us, now_us, impl_->have_expected,
                                          impl_->expected_sequence, packet.sequence, 12)) {
        impl_->packets.clear();
        impl_->queued_input_samples=0;impl_->input_overflowed=false;
        impl_->primed = false;
        impl_->have_expected = false;
        impl_->jitter_hold_until_us = now_us + impl_->resync_jitter_hold_us;
        impl_->reset_epoch_requested.store(true);
    }
    if (impl_->have_expected && int16_t(packet.sequence - impl_->expected_sequence) < 0) {
        impl_->late_drops++;
        return;
    }
    const auto duplicate = std::find_if(
        impl_->packets.begin(), impl_->packets.end(), [&packet](const Impl::Packet& item) {
            return item.ssrc == packet.ssrc && item.sequence == packet.sequence;
        });
    if (duplicate != impl_->packets.end())
        return;
    auto position = std::find_if(impl_->packets.begin(), impl_->packets.end(), [&packet](const Impl::Packet& item) {
        return int16_t(packet.sequence - item.sequence) < 0;
    });
    impl_->queued_input_samples+=packet.samples;
    impl_->packets.insert(position, std::move(packet));
    while(impl_->packets.size()>kMaxPackets || impl_->queued_input_samples>kInputQueueSamples) {
        impl_->queued_input_samples-=impl_->packets.front().samples;
        impl_->packets.pop_front();impl_->queue_drops++;impl_->input_queue_drops++;
        impl_->input_overflowed=true;
    }
    impl_->cv.notify_one();
}

void AudioPipeline::set_sender_report(uint32_t ssrc, uint64_t ntp_us, uint32_t rtp_timestamp) {
    if (impl_->audio_ssrc.load() == 0 || impl_->audio_ssrc.load() != ssrc)
        return;
    {
        std::lock_guard<std::mutex> lock(impl_->sender_report_mutex);
        if (impl_->audio_ssrc.load() != ssrc)
            return;
        impl_->sr_ntp_us = ntp_us;
        impl_->sr_rtp_timestamp = rtp_timestamp;
        impl_->have_sender_report.store(true, std::memory_order_release);
    }
    impl_->log("RTCP_SR ssrc=" + std::to_string(ssrc) + " ntpUs=" + std::to_string(ntp_us) +
               " rtp=" + std::to_string(rtp_timestamp));
}

int64_t AudioPipeline::playback_ntp_us() const {
    if (!impl_->output_ready.load())
        return -1;
    std::lock_guard<std::mutex> lock(impl_->sender_report_mutex);
    if (!impl_->have_sender_report.load(std::memory_order_acquire))
        return -1;
    const uint64_t observed_at_us =
        impl_->played_samples_observed_at_us.load(std::memory_order_acquire);
    const uint64_t clock_us = impl_->output_started.load(std::memory_order_acquire)
        ? monotonic_us() : observed_at_us;
    const uint64_t played = opennow::audio::EstimatePlayedSamples(
        impl_->played_samples.load(std::memory_order_relaxed), observed_at_us,
        clock_us, impl_->submitted_samples.load(std::memory_order_relaxed),
        kSampleRate);
    const auto position = impl_->playback_timeline.position(played);
    if (!position || position->ssrc != impl_->audio_ssrc.load())
        return -1;
    return static_cast<int64_t>(impl_->sr_ntp_us) +
           opennow::audio::RtpDeltaToUs(
               position->timestamp, impl_->sr_rtp_timestamp, kSampleRate);
}

AudioLevels AudioPipeline::levels() const {
    AudioLevels levels;
    levels.packets_received = impl_->packets_rx.load();
    levels.payload_bytes_received = impl_->payload_bytes_rx.load();
    levels.packets_decoded = impl_->packets_decoded.load();
    levels.decode_errors = impl_->decode_errors.load();
    levels.samples_decoded = impl_->samples_decoded.load();
    levels.input_peak = impl_->last_input_peak.load();
    levels.input_rms = impl_->last_input_rms.load();
    levels.output_peak = impl_->last_output_peak.load();
    levels.output_rms = impl_->last_output_rms.load();
    levels.output_ready = impl_->output_ready.load();
    levels.output_started = impl_->output_started.load();
    levels.last_output_error = impl_->last_output_error.load();
    levels.output_errors = impl_->output_errors.load();
    levels.samples_submitted = impl_->submitted_samples.load();
    levels.samples_played = impl_->played_samples.load();
    levels.output_drops = impl_->output_throttle_drops.load();
    levels.underruns = impl_->underruns.load();
    levels.input_queue_drops=impl_->input_queue_drops.load();levels.late_drops=impl_->late_drops.load();
    levels.sequence_gaps=impl_->sequence_gaps.load();levels.conceal_samples=impl_->conceal_samples.load();
    levels.sequence_reanchors=impl_->sequence_reanchors.load();levels.pacing_waits=impl_->pacing_waits.load();
    levels.receive_gap_max_us=impl_->receive_gap_max_us.load();levels.worker_gap_max_us=impl_->worker_gap_max_us.load();
    levels.decode_max_us=impl_->decode_max_us.load();levels.buffered_ms=impl_->output_buffered_samples.load()/48;
    levels.target_buffer_ms=static_cast<unsigned>(impl_->target_buffer_ms.load());levels.packet_duration_us=impl_->packet_duration_us.load();
    levels.worker_priority=impl_->scheduling.actual();levels.worker_priority_error=impl_->scheduling.error();
    levels.diagnostic_drops=impl_->diagnostic_writer.dropped();levels.diagnostic_write_errors=impl_->diagnostic_writer.failures();
    levels.diagnostic_write_max_us=impl_->diagnostic_writer.write_peak_us();
    return levels;
}

std::string AudioPipeline::debug_info() const {
    size_t queue_size = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        queue_size = impl_->packets.size();
    }
    const uint64_t played = impl_->played_samples.load();
    const uint64_t submitted = impl_->submitted_samples.load();
    const uint64_t queued = submitted > played ? submitted - played : 0;
    char text[430];
    std::snprintf(text, sizeof(text),
        "Audio rx/decoded/errors: %llu/%llu/%llu\nAudio queue packets/ms: %zu/%llu  gaps/late/drop: %llu/%llu/%llu\nAudio samples played/queued PLC/underrun RED/recovered/reset: %llu/%llu %llu/%llu %llu/%llu/%llu\nAudio level in peak/rms -> out peak/rms, gain/limit: %d/%d -> %d/%d, %.2fx/%llu",
        static_cast<unsigned long long>(impl_->packets_rx.load()),
        static_cast<unsigned long long>(impl_->packets_decoded.load()),
        static_cast<unsigned long long>(impl_->decode_errors.load()), queue_size,
        static_cast<unsigned long long>(queued * 1000 / kSampleRate),
        static_cast<unsigned long long>(impl_->sequence_gaps.load()),
        static_cast<unsigned long long>(impl_->late_drops.load()),
        static_cast<unsigned long long>(impl_->queue_drops.load()),
        static_cast<unsigned long long>(played), static_cast<unsigned long long>(queued),
        static_cast<unsigned long long>(impl_->plc_frames.load()),
        static_cast<unsigned long long>(impl_->underruns.load()),
        static_cast<unsigned long long>(impl_->red_packets.load()),
        static_cast<unsigned long long>(impl_->red_recoveries.load()),
        static_cast<unsigned long long>(impl_->epoch_resets.load()),
        impl_->last_input_peak.load(), impl_->last_input_rms.load(),
        impl_->last_output_peak.load(), impl_->last_output_rms.load(),
        static_cast<double>(impl_->last_applied_gain_x100.load()) / 100.0,
        static_cast<unsigned long long>(impl_->limited_frames.load()));
    return text;
}
