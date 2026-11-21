#include "stream/BoundedDiagnosticWriter.hpp"
#include <cassert>
#include <stdexcept>
using opennow::stream::BoundedDiagnosticWriter;
int main() {
    BoundedDiagnosticWriter writer;
    assert(!writer.submit("inactive"));
    std::mutex mutex;std::condition_variable wake;
    bool entered=false,release=false;std::string output;
    assert(writer.start([&](const std::string& batch) {
        std::unique_lock lock(mutex);entered=true;wake.notify_all();
        assert(wake.wait_for(lock,std::chrono::seconds(3),[&]{return release;}));
        output+=batch;return true;
    }));
    assert(writer.submit("first\n"));
    {std::unique_lock lock(mutex);assert(wake.wait_for(lock,std::chrono::seconds(2),[&]{return entered;}));}
    // The sink is deliberately stuck. Producers still enqueue without waiting
    // for it, and saturation drops debug data rather than growing memory.
    for(int i=0;i<128;++i) assert(writer.submit("pending\n"));
    assert(!writer.submit("full\n"));
    assert(!writer.submit(std::string(BoundedDiagnosticWriter::kRecordBytes+1,'x')));
    assert(writer.dropped()==2);
    {std::lock_guard lock(mutex);release=true;}wake.notify_all();writer.stop();
    assert(output.starts_with("first\n") && std::count(output.begin(),output.end(),'\n')==129);
    assert(writer.failures()==0 && !writer.submit("after stop"));

    output.clear();
    assert(writer.start([&](const std::string& batch){output+=batch;return true;}));
    assert(writer.submit("shutdown flush\n"));writer.stop();
    assert(output=="shutdown flush\n" && writer.dropped()==0);
    assert(writer.start([](const std::string&){throw std::runtime_error("test sink");return false;}));
    assert(writer.submit("failure"));writer.stop();assert(writer.failures()==1);
    assert(writer.start([](const std::string&){return false;}));
    assert(writer.submit("failure"));writer.stop();assert(writer.failures()==1);
    // Bound both bytes in flight and the complete session file, not just the
    // record count. Drain one 64 KiB batch at a time until the 512 KiB cap.
    std::size_t written=0;
    assert(writer.start([&](const std::string& batch) {
        std::lock_guard lock(mutex);written+=batch.size();wake.notify_all();return true;
    }));
    const std::string large(BoundedDiagnosticWriter::kRecordBytes,'x');
    for(std::size_t batch=0;batch<8;++batch) {
        for(int i=0;i<32;++i) assert(writer.submit(large));
        {std::unique_lock lock(mutex);assert(wake.wait_for(lock,std::chrono::seconds(3),[&]{
            return written==(batch+1)*BoundedDiagnosticWriter::kQueueBytes;
        }));}
    }
    assert(!writer.submit("cap reached"));writer.stop();
    assert(written==BoundedDiagnosticWriter::kSessionBytes && writer.dropped()==1);
}
