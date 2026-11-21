#pragma once
#include "atomic_file_replace.hpp"
#include "app_version.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cloudgenshin {
// Only call with source-defined event names and numeric codes/counters. Never
// pass server bodies, identifiers, credentials, endpoints, QR content or SDP.
class SafeEventLog {
public:
    explicit SafeEventLog(std::string version) : version_(std::move(version)) {}
    void append(std::string_view event, int code=0, long http=0) {
        append_impl(event,code,http,false);
    }
    // Evidence events (game/SDK protocol milestones) must survive both the
    // byte-bounded snapshot and the event-count ring. Dropping them once hid an
    // entire `cloud_get_data` exchange, which made a real request look like
    // "the game never asked". Only fixed source-defined names may pass here.
    void append_kept(std::string_view event, int code=0, long http=0) {
        append_impl(event,code,http,true);
    }
    std::string recent(std::size_t count=8) const {
        std::lock_guard lock(mutex_);
        std::string result;
        for (auto i=events_.size()>count ? events_.size()-count : 0; i<events_.size(); ++i)
            result+=events_[i].line+'\n';
        return result;
    }
    std::string snapshot() const {
        // Copy the bounded history, then release the producer lock before
        // budgeting, sorting and formatting. SD autosaves must not stall RTC
        // callbacks behind quadratic duplicate checks on thousands of lines.
        const auto copy=capture();
        const auto& events=copy.events;
        const auto& startup=copy.startup;
        const auto& kept=copy.kept;
        std::string result=header();
        result+="run_started_unix_seconds="+std::to_string(run_started_)+"\n";
        constexpr std::size_t kSnapshotBytes=128*1024;
        // Kept evidence is never trimmed. Everything else shares what remains of
        // the byte budget, so a chatty counter cannot hide a protocol milestone.
        std::size_t kept_bytes=0;
        for(const auto& entry:kept) kept_bytes+=entry.second.size()+1;
        const std::size_t budget=kept_bytes<kSnapshotBytes ? kSnapshotBytes-kept_bytes : 0;
        const auto first_recent=copy.sequence-static_cast<unsigned>(events.size())+1;
        std::size_t from=0;
        while(from<events.size() && first_recent+from<=startup.size()) ++from;
        std::size_t bytes=result.size()+64;
        for(const auto& line:startup) bytes+=line.size()+1;
        for(std::size_t i=from;i<events.size();++i) {
            if(events[i].kept) continue;
            bytes+=events[i].line.size()+1;
        }
        while(bytes>budget && from<events.size()) {
            if(!events[from].kept) bytes-=events[from].line.size()+1;
            ++from;
        }
        // Merge kept evidence back into chronological order. Line numbers are
        // monotonic per session, so ordering stays truthful after trimming.
        std::vector<std::pair<unsigned,const std::string*>> ordered;
        ordered.reserve(startup.size()+events.size()-from+kept.size());
        for(std::size_t i=0;i<startup.size();++i) ordered.emplace_back(static_cast<unsigned>(i+1),&startup[i]);
        for(const auto& entry:kept) ordered.emplace_back(entry.first,&entry.second);
        for(std::size_t i=from;i<events.size();++i)
            if(!events[i].kept) ordered.emplace_back(events[i].sequence,&events[i].line);
        std::sort(ordered.begin(),ordered.end(),
            [](const auto& a,const auto& b) {return a.first<b.first;});
        // Report only lines that are truly absent: a kept milestone inside the
        // trimmed window is still printed below, so it must not be counted.
        const unsigned first_tail=first_recent+static_cast<unsigned>(from);
        const unsigned tail_begin=static_cast<unsigned>(startup.size())+1;
        unsigned omitted=first_tail>tail_begin ? first_tail-tail_begin : 0;
        if(omitted) {
            unsigned shown=0;
            for(const auto& entry:kept)
                if(entry.first>=tail_begin && entry.first<first_tail) ++shown;
            omitted-=std::min(omitted,shown);
        }
        if(omitted) result+="omitted_event_count="+std::to_string(omitted)+'\n';
        for(const auto& entry:ordered) result+=*entry.second+'\n';
        return result;
    }
    struct Batch {std::string header,text;unsigned last=0;};
    Batch since(unsigned cursor) const {
        const auto copy=capture(cursor);
        Batch batch{header()+
            "run_started_unix_seconds="+std::to_string(run_started_)+"\n",{},copy.sequence};
        if(cursor>=copy.sequence) return batch;
        std::vector<std::pair<unsigned,const std::string*>> ordered;
        for(unsigned i=0;i<copy.startup.size();++i)
            if(i+1>cursor) ordered.emplace_back(i+1,&copy.startup[i]);
        for(const auto& entry:copy.events)
            if(entry.sequence>cursor) ordered.emplace_back(entry.sequence,&entry.line);
        for(const auto& entry:copy.kept)
            if(entry.first>cursor) ordered.emplace_back(entry.first,&entry.second);
        std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.first<b.first;});
        unsigned previous=cursor;
        for(const auto& entry:ordered) {
            if(entry.first==previous) continue;
            if(entry.first>previous+1)
                batch.text+="omitted_event_count="+std::to_string(entry.first-previous-1)+'\n';
            batch.text+=*entry.second+'\n';previous=entry.first;
        }
        return batch;
    }
    bool save(const std::string& path,std::string (*formatter)(const std::string&)=nullptr) const {
        // Serialize saves independently of event and peer locks. A failed SD
        // write must leave the previous complete file and the RAM log usable.
        std::lock_guard lock(file_mutex_);
        const auto raw=snapshot();
        const auto text=formatter ? formatter(raw) : raw;
        const auto temporary=path+".tmp";
        std::ofstream file(temporary,std::ios::trunc|std::ios::binary);
        if (!file) return false;
        file<<text; file.flush();
        const bool flushed=file.good();
        file.close();
        if (!flushed || file.fail()) {std::remove(temporary.c_str());return false;}
        return opennow::storage::ReplaceWithTemporaryFile(temporary,path);
    }
private:
    struct Entry {
        unsigned sequence=0;
        std::string line;
        bool kept=false;
    };
    struct Captured {
        std::deque<Entry> events;
        std::deque<std::string> startup;
        std::deque<std::pair<unsigned,std::string>> kept;
        unsigned sequence;
    };
    Captured capture(unsigned cursor=0) const {
        std::lock_guard lock(mutex_);
        if(!cursor) return {events_,startup_,kept_,sequence_};
        Captured copy{{},{},{},sequence_};
        if(cursor>=sequence_) return copy;
        if(cursor<startup_.size()) copy.startup=startup_;
        // Most archive writes need only a few new lines, not another copy of
        // the complete recent ring. Compare sequence numbers under the lock;
        // merge and format the selected immutable copies after releasing it.
        for(const auto& entry:events_)
            if(entry.sequence>cursor) copy.events.push_back(entry);
        for(const auto& entry:kept_)
            if(entry.first>cursor) copy.kept.push_back(entry);
        return copy;
    }
    // Kept evidence is small and bounded independently so a chatty counter can
    // never push the whole block past the snapshot budget.
    static constexpr std::size_t kMaximumKept=512;
    void append_impl(std::string_view event, int code, long http, bool keep) {
        if (event.empty() || event.size()>96 || !std::all_of(event.begin(),event.end(),[](char c) {
            return (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_';
        })) event="UNSAFE_EVENT_TEXT_SUPPRESSED";
        const auto seconds=std::chrono::duration_cast<std::chrono::seconds>(Clock::now()-started_).count();
        std::lock_guard lock(mutex_);
        const unsigned sequence=++sequence_;
        std::string line=std::to_string(sequence)+" t="+std::to_string(seconds)+"s "+std::string(event);
        line+=" code="+std::to_string(code);
        if (http) line+=" http="+std::to_string(http);
        // Preserve bounded boot/auth/transport evidence even after several
        // minutes of counters evict it from the recent-event ring.
        if(sequence<=128) startup_.push_back(line);
        // RAM ring is bounded by event count; snapshots also enforce the
        // monitor's byte budget. The archive preserves incremental history.
        if (events_.size()==2048) events_.pop_front();
        if(keep) {
            // Startup already retains the first 128 lines verbatim.
            if(sequence>128) {
                if(kept_.size()==kMaximumKept) kept_.pop_front();
                kept_.emplace_back(sequence,line);
            }
            events_.push_back(Entry{sequence,std::move(line),true});
            return;
        }
        events_.push_back(Entry{sequence,std::move(line),false});
    }
    std::string header() const {
        return std::string(opennow::kNativeAppName)+" native alpha "+version_+" / safe events only\n";
    }
    using Clock=std::chrono::steady_clock;
    const std::string version_;
    const Clock::time_point started_=Clock::now();
    const long long run_started_=std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    mutable std::mutex mutex_,file_mutex_;
    std::deque<Entry> events_;
    std::deque<std::string> startup_;
    std::deque<std::pair<unsigned,std::string>> kept_;
    unsigned sequence_=0;
};
} // namespace cloudgenshin
