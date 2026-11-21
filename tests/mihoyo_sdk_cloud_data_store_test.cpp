#include "mihoyo/sdk_cloud_data_store.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using cloudgenshin::sdk::CloudDataStore;
using Json=nlohmann::json;
struct Temp {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("yuanshi-sdk-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp(){std::filesystem::create_directory(path);}
    ~Temp(){std::filesystem::remove_all(path);}
    std::string home() const{return path.string();}
};
std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
void Write(const std::filesystem::path& path,const std::string& bytes) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<bytes;assert(out.good());
}
Json Data(std::string value) {return Json{{"cloud_local_data_123",std::move(value)}};}
}
int main() {
    Temp temp;
    std::mutex events_mutex;
    std::vector<std::string> events;
    const auto sink=[&](const char* tag,int code){
        std::lock_guard lock(events_mutex);events.push_back(std::string(tag)+"="+std::to_string(code));
    };
    {
        CloudDataStore first(temp.home(),"4","1","100001",sink);
        assert(first.restored().empty());
        assert(std::filesystem::is_empty(temp.path)); // Merely starting does not write.
        for(int i=0;i<100;++i) first.update(Data("opaque-game-config-"+std::to_string(i)));
        first.stop(); // Flush latest even when shutdown precedes the burst timer.
    }
    {
        CloudDataStore next(temp.home(),"4","1","100001",sink);
        assert(next.restored()==Data("opaque-game-config-99"));
        next.update(Json{{"account_id","never-save-account"},{"channel_token","never-save-token"}});
        next.update(Json{{"../escape","never-save-path"}});
        next.update(Json{{"cloud_local_data_123",Json{{"buttons",1}}}}); // Unknown value shape.
        next.update(Data(std::string(256*1024,'x'))); // Bounded, no replacement.
    }
    {
        CloudDataStore next(temp.home(),"4","1","100001",sink);
        assert(next.restored()==Data("opaque-game-config-99"));
        next.update(Data("second-snapshot"));
    }
    {
        CloudDataStore next(temp.home(),"4","1","100001",sink);
        assert(next.restored()==Data("second-snapshot"));
    }
    // Filenames have keyed hashes, never the account id or the game's key.
    std::filesystem::path latest;
    for(const auto& file:std::filesystem::directory_iterator(temp.path)) {
        const auto name=file.path().filename().string(),bytes=Read(file.path());
        assert(name.find("100001")==std::string::npos && name.find("cloud_local_data")==std::string::npos);
        for(const auto* text:{"opaque-game-config","second-snapshot","never-save-account","never-save-token","never-save-path"})
            assert(bytes.find(text)==std::string::npos);
        if(name.ends_with("-0.vault")) latest=file.path();
    }
    assert(!latest.empty());
    // A path collision with a directory must never turn into a rename/delete
    // of that directory, and the other complete slot remains readable.
    {Temp collision;
     {CloudDataStore first(collision.home(),"4","1","100001");first.update(Data("safe-original"));}
     std::filesystem::path slot;
     for(const auto& file:std::filesystem::directory_iterator(collision.path))
         if(file.path().string().ends_with("-0.vault")) slot=file.path();
     assert(!slot.empty());std::filesystem::remove(slot);std::filesystem::create_directory(slot);
     Write(slot/"user-file.txt","keep-me");
     {CloudDataStore next(collision.home(),"4","1","100001");next.update(Data("not-saved"));}
     assert(std::filesystem::is_directory(slot) && Read(slot/"user-file.txt")=="keep-me");
     {CloudDataStore next(collision.home(),"4","1","100001");assert(next.restored()==Data("safe-original"));}}
    const auto good=Read(latest);
    auto corrupted=good;corrupted.back()^=1;Write(latest,corrupted);
    {CloudDataStore recovered(temp.home(),"4","1","100001",sink);
     assert(recovered.restored()==Data("opaque-game-config-99"));
     recovered.update(Data("recovered-snapshot"));}
    {CloudDataStore recovered(temp.home(),"4","1","100001");assert(recovered.restored()==Data("recovered-snapshot"));}
    // Independent account and channel, never replay another user's data.
    {CloudDataStore account(temp.home(),"4","1","100002");
     assert(account.restored().empty());account.update(Data("other-account"));}
    {CloudDataStore channel(temp.home(),"4","2","100001");
     assert(channel.restored().empty());channel.update(Data("other-channel"));}
    {CloudDataStore account(temp.home(),"4","1","100001");assert(account.restored()==Data("recovered-snapshot"));}
    const auto key=temp.path/"game-sdk-data.key";
    std::filesystem::copy_file(key,key.string()+".bak",std::filesystem::copy_options::overwrite_existing);
    Write(key,std::string(72,'x'));
    {CloudDataStore recovered(temp.home(),"4","1","100001");assert(recovered.restored()==Data("recovered-snapshot"));}
    Write(key,Read(key.string()+".bak"));
    // Authenticated wrong-account ciphertext cannot be adopted, even if copied
    // into the right hash-named slot. Keep the other slot for recovery.
    std::filesystem::path other;
    for(const auto& file:std::filesystem::directory_iterator(temp.path)) {
        if(!file.path().string().ends_with("-1.vault")) continue;
        const auto zero=file.path().string().substr(0,file.path().string().size()-8)+"-0.vault";
        if(zero!=latest.string()) {other=file.path();break;}
    }
    assert(!other.empty());Write(latest,Read(other));
    {CloudDataStore isolated(temp.home(),"4","1","100001");assert(isolated.restored()==Data("opaque-game-config-99"));}
    // Other profiles and invalid scopes never create files or start writers.
    {Temp separate;
     for(const auto app:{"8","12","../escape"}) {CloudDataStore no(separate.home(),app,"1","100001");no.update(Data("ignored"));}
     {CloudDataStore no(separate.home(),"4","1","not-numeric");no.update(Data("ignored"));}
     assert(std::filesystem::is_empty(separate.path));}
    // A healthy session persists in the background without waiting for exit.
    {Temp background;CloudDataStore live(background.home(),"4","1","100001");live.update(Data("while-playing"));
     bool ready=false;
     for(int i=0;i<200 && !ready;++i) {
         std::this_thread::sleep_for(std::chrono::milliseconds(10));
         CloudDataStore observer(background.home(),"4","1","100001");ready=observer.restored()==Data("while-playing");
     }
     assert(ready);}
    // Missing/unwritable directory is nonfatal and does not hang shutdown.
    {Temp recovery;
     const Json own{{"cloud_local_data_100001","own-game-config"}},foreign{{"cloud_local_data_200002","other-game-config"}};
     {CloudDataStore seeded(recovery.home(),"4","1","100001");seeded.update(own);}
     {CloudDataStore saved(recovery.home(),"4","1","100001");
      assert(saved.save_session_baseline((recovery.path/"game-sdk-hk4e-session-start.json").string()));
      const auto candidates=saved.controller_recovery_baselines();assert(candidates.size()==1 && candidates[0]==own);}
     const auto bound=Read(recovery.path/"game-sdk-hk4e-session-start.json");
     Write(recovery.path/"game-sdk-hk4e-session-start.json",Json{{"version",1},{"data",foreign}}.dump());
     {CloudDataStore saved(recovery.home(),"4","1","100001");assert(saved.controller_recovery_baselines()==std::vector<Json>{own});}
     Write(recovery.path/"game-sdk-hk4e-session-start.json.bak",bound);
     {CloudDataStore saved(recovery.home(),"4","1","100001");assert(saved.controller_recovery_baselines()==std::vector<Json>{own});}
     {CloudDataStore other(recovery.home(),"4","1","200002");assert(other.controller_recovery_baselines().empty());}
     const Json channel_data{{"cloud_local_data_100001","channel-2-config"}};
     {CloudDataStore channel(recovery.home(),"4","2","100001");channel.update(channel_data);}
     {CloudDataStore channel(recovery.home(),"4","2","100001");assert(channel.controller_recovery_baselines()==std::vector<Json>{channel_data});}
     // A later default overwrites the latest slot, but older authenticated
     // generations remain available as controller-only recovery candidates.
     {CloudDataStore changed(recovery.home(),"4","1","100001");changed.update(Json{{"cloud_local_data_100001","later-default"}});}
     {CloudDataStore changed(recovery.home(),"4","1","100001");const auto candidates=changed.controller_recovery_baselines();
      assert(std::find(candidates.begin(),candidates.end(),own)!=candidates.end());}}
    {CloudDataStore failed(temp.home()+"/absent","4","1","100001",sink);failed.update(Data("unsaved"));}
    for(const auto& line:events) {
        assert(line.find("100001")==std::string::npos && line.find("opaque")==std::string::npos);
        assert(line.find("cloud_local_data")==std::string::npos);
    }
}
