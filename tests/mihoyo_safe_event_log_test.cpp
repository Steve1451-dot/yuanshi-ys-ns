#include "mihoyo/safe_event_log.hpp"
#include <cassert>
#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>
#include <sstream>

std::string read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
int main() {
    using cloudgenshin::SafeEventLog;
    SafeEventLog log("0.2.3");
    log.append("LOGIN_OK",0,200);
    log.append("VIDEO_DECODE_ERRORS",-42);
    log.append("wss://example.invalid/secret");
    log.append("QR=secret\nCookie: secret");
    assert(log.snapshot().find("native alpha 0.2.3")!=std::string::npos);
    assert(log.snapshot().starts_with("yuanshi launcher native alpha 0.2.3 / safe events only\n"));
    assert(log.since(0).header.starts_with("yuanshi launcher native alpha 0.2.3 / safe events only\n"));
    assert(log.snapshot().find("run_started_unix_seconds=")!=std::string::npos);
    assert(log.snapshot().find("secret")==std::string::npos);
    assert(log.snapshot().find("VIDEO_DECODE_ERRORS code=-42")!=std::string::npos);
    assert(log.recent(0).empty());
    assert(log.recent(1).find("UNSAFE_EVENT_TEXT_SUPPRESSED")!=std::string::npos);
    std::vector<std::thread> workers;
    for (int i=0;i<4;++i) workers.emplace_back([&log] {
        for (int j=0;j<200;++j) log.append("VIDEO_UNITS_RECEIVED",j);
    });
    for (auto& worker:workers) worker.join();
    const auto lines=log.recent(3000);
    assert(std::count(lines.begin(),lines.end(),'\n')==804);
    const auto full=log.snapshot();
    assert(full.find("LOGIN_OK")!=std::string::npos && lines.find("LOGIN_OK")!=std::string::npos);
    assert(full.find("LOGIN_OK")==full.rfind("LOGIN_OK") && full.find("secret")==std::string::npos);
    assert(std::count(full.begin(),full.end(),'\n')==806);
    SafeEventLog overlapping("test");
    for(int i=0;i<2500;++i) overlapping.append("COUNTER",i);
    const auto overlap=overlapping.snapshot();
    assert(std::count(overlap.begin(),overlap.end(),'\n')==128+2048+3);
    assert(overlap.find("omitted_event_count=")!=std::string::npos);
    SafeEventLog verbose("test");verbose.append("LOGIN_RESTORED");
    for(int i=0;i<3000;++i) verbose.append(std::string(96,'A'),i);
    const auto bounded=verbose.snapshot();assert(bounded.size()<=128*1024);
    assert(bounded.find("LOGIN_RESTORED")!=std::string::npos && bounded.find("code=2999")!=std::string::npos);
    assert(bounded.find("omitted_event_count=")!=std::string::npos);
    // Evidence events must survive both the ring and the byte budget. A dropped
    // cloud_get_data exchange once made a real request look absent.
    SafeEventLog evidence("test");
    evidence.append_kept("GAME_SDK_CUSTOM_GET_LOCAL_DATA");
    for(int i=0;i<2600;++i) evidence.append("VIDEO_UNITS_RECEIVED",i);
    evidence.append_kept("GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT");
    for(int i=0;i<300;++i) evidence.append(std::string(96,'B'),i);
    const auto kept=evidence.snapshot();
    assert(kept.size()<=128*1024);
    assert(kept.find("GAME_SDK_CUSTOM_GET_LOCAL_DATA")!=std::string::npos);
    assert(kept.find("GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT")!=std::string::npos);
    // Chronological order is preserved after trimming.
    assert(kept.find("GAME_SDK_CUSTOM_GET_LOCAL_DATA")<
        kept.find("GAME_SDK_NATIVE_KEYBIND_RESTORE_SENT"));
    // Incremental archive re-emits kept evidence even after ring eviction.
    SafeEventLog incremental("test");
    const unsigned first=incremental.since(0).last;
    incremental.append_kept("GAME_SDK_CLOUD_DATA_READ_HIT");
    for(int i=0;i<2200;++i) incremental.append("COUNTER",i);
    const auto batch=incremental.since(first);
    assert(batch.text.find("GAME_SDK_CLOUD_DATA_READ_HIT")!=std::string::npos);
    assert(incremental.since(batch.last).text.empty());
    // Kept evidence in an evicted middle range is emitted once and in order,
    // with an accurate gap count rather than a late duplicate at batch end.
    SafeEventLog chronological("test");
    for(int i=0;i<150;++i) chronological.append("STARTUP");
    chronological.append_kept("SAVED_MILESTONE");
    for(int i=0;i<2500;++i) chronological.append("TAIL");
    const auto sorted=chronological.since(0);
    assert(sorted.text.find("SAVED_MILESTONE")<sorted.text.find("TAIL"));
    assert(sorted.text.find("SAVED_MILESTONE")==sorted.text.rfind("SAVED_MILESTONE"));
    const auto verify=[](const std::string& text) {
        std::istringstream in(text);std::string line;unsigned previous=0;
        while(std::getline(in,line)) {
            if(line.empty() || line[0]<'0' || line[0]>'9') continue;
            const auto number=static_cast<unsigned>(std::stoul(line));
            assert(number>previous);previous=number;
        }
    };
    verify(sorted.text);
    // Concurrent snapshots/archives must be self-consistent while the media
    // owner is appending; no unlocked references into the live ring survive.
    std::atomic_bool producing{true};
    std::thread producer([&]{for(int i=0;i<6000;++i) {
        if(i%127==0) chronological.append_kept("MILESTONE",i);
        else chronological.append("LIVE_COUNTER",i);
    }producing=false;});
    unsigned cursor=sorted.last;
    do {
        const auto live=chronological.since(cursor);verify(live.text);cursor=live.last;
        verify(chronological.snapshot());
    }while(producing.load());
    producer.join();
    const auto directory=std::filesystem::temp_directory_path()/
        ("cloudgenshin-safe-log-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const auto path=directory/"safe.log";
    assert(log.save(path.string()));
    assert(read(path)==log.snapshot());
    log.append("STREAM_DIAGNOSTICS_SNAPSHOT");
    assert(log.save(path.string()));
    assert(read(path)==log.snapshot());
    assert(!log.save((directory/"missing"/"safe.log").string()));
    assert(!log.recent(1).empty());
    // Failed installation must not destroy the previous complete file.
    assert(!opennow::storage::ReplaceWithTemporaryFile((directory/"missing.tmp").string(),path.string()));
    assert(read(path)==log.snapshot());
    std::filesystem::remove(path);
    std::filesystem::remove(directory);
}
