#pragma once
#include "stream/WorkerScheduling.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace opennow::stream {
// Optional text diagnostics only. No file/socket I/O or sink callbacks on the
// producer. A stalled SD card loses debug records, not media-buffer ownership.
class BoundedDiagnosticWriter {
public:
    using Sink=std::function<bool(const std::string&)>;
    static constexpr std::size_t kQueueBytes=64*1024,kQueueRecords=128;
    static constexpr std::size_t kRecordBytes=2048,kSessionBytes=512*1024;
    ~BoundedDiagnosticWriter() {stop();}
    bool start(Sink sink) {
        stop();
        {std::lock_guard lock(mutex_);
         sink_=std::move(sink);pending_.clear();queued_bytes_=accepted_bytes_=0;
         dropped_=0;failures_=0;write_peak_us_=0;stopping_=false;active_=true;}
        try {worker_=std::thread([this]{run();});}
        catch(...) {std::lock_guard lock(mutex_);active_=false;return false;}
        return true;
    }
    bool submit(std::string record) {
      try {
        std::lock_guard lock(mutex_);
        if(!active_) return false;
        if(record.empty() || record.size()>kRecordBytes || pending_.size()>=kQueueRecords ||
           record.size()>kQueueBytes-queued_bytes_ || record.size()>kSessionBytes-accepted_bytes_) {
            ++dropped_;return false;
        }
        const auto bytes=record.size();pending_.push_back(std::move(record));
        queued_bytes_+=bytes;accepted_bytes_+=bytes;wake_.notify_one();return true;
      } catch(...) {++dropped_;return false;}
    }
    // Lifecycle owner calls after producers have stopped. Shutdown flushes the
    // final bounded batch; it may wait for SD I/O, unlike submit().
    void stop() {
        {std::lock_guard lock(mutex_);active_=false;stopping_=true;}
        wake_.notify_one();
        if(worker_.joinable()) worker_.join();
    }
    std::uint64_t dropped() const {return dropped_.load();}
    std::uint64_t failures() const {return failures_.load();}
    std::uint64_t write_peak_us() const {return write_peak_us_.load();}
private:
    void run() {
        using Clock=std::chrono::steady_clock;
        WorkerScheduling scheduling;scheduling.apply(WorkerRole::Background);
        std::unique_lock lock(mutex_);
        while(true) {
            wake_.wait(lock,[this]{return stopping_ || !pending_.empty();});
            if(pending_.empty() && stopping_) break;
            // Coalesce writes even with frequent events; no busy polling.
            wake_.wait_until(lock,Clock::now()+std::chrono::milliseconds(250),[this]{return stopping_;});
            const auto bytes=queued_bytes_;
            std::deque<std::string> records;records.swap(pending_);queued_bytes_=0;
            lock.unlock();
            const auto started=Clock::now();
            try {
                std::string batch;batch.reserve(bytes);
                for(const auto& record:records) batch+=record;
                if(!sink_ || !sink_(batch)) ++failures_;
            } catch(...) {++failures_;}
            const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-started).count();
            write_peak_us_.store(std::max(write_peak_us_.load(),static_cast<std::uint64_t>(elapsed)));
            lock.lock();
        }
    }
    Sink sink_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> pending_;
    std::size_t queued_bytes_=0,accepted_bytes_=0;
    bool active_=false,stopping_=true;
    std::thread worker_;
    std::atomic_uint64_t dropped_{0},failures_{0},write_peak_us_{0};
};
}
