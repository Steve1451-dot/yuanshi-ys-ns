#include "mihoyo/sdk_cloud_data_store.hpp"
#include "mihoyo/sdk_json.hpp"
#include "atomic_file_replace.hpp"
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <thread>
#include <sys/stat.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace cloudgenshin::sdk {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaximum = 256 * 1024;
constexpr char kHeader[] = "YSDKD001";
constexpr char kKeyHeader[] = "YSDKK001";
std::mutex disk_mutex;
enum class ReadStatus {Missing, Ready, Failed};
bool Digits(std::string_view text, std::size_t maximum = 32) {
    return !text.empty() && text.size() <= maximum &&
        std::all_of(text.begin(), text.end(), [](char c) {return c >= '0' && c <= '9';});
}
bool GameKey(std::string_view key) {
    constexpr std::string_view prefix = "cloud_local_data_";
    return key.starts_with(prefix) && Digits(key.substr(prefix.size()));
}
std::optional<Json> GameData(const Json& data) {
    if (!data.is_object() || data.size() > 32) return {};
    Json result = Json::object();
    std::size_t bytes = 0;
    for (auto it = data.begin(); it != data.end(); ++it) {
        // Never persist account_id, SDK login responses, tokens or arbitrary
        // server-directed filenames. Preserve the opaque game string exactly.
        if (!GameKey(it.key())) continue;
        if (!it.value().is_string()) return {};
        bytes += it.key().size() + it.value().get_ref<const std::string&>().size();
        if (bytes > kMaximum - 1024) return {};
        result[it.key()] = it.value();
    }
    return result;
}
ReadStatus Read(const std::string& path, std::string& bytes, std::size_t maximum) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return errno == ENOENT ? ReadStatus::Missing : ReadStatus::Failed;
    std::string buffer(maximum + 1, '\0');
    const auto size = std::fread(buffer.data(), 1, buffer.size(), file);
    const bool good = !std::ferror(file) && size <= maximum;
    const bool closed = std::fclose(file) == 0;
    if (!good || !closed) return ReadStatus::Failed;
    buffer.resize(size); bytes = std::move(buffer); return ReadStatus::Ready;
}
bool Write(const std::string& path, const std::string& bytes) {
    // Do not let the FAT rename fallback move a user-created directory aside
    // as if it were a damaged snapshot or backup file.
    for (const auto& target : {path, path + ".bak"}) {
        struct stat info{};
        if (stat(target.c_str(), &info) == 0) {if (!S_ISREG(info.st_mode)) return false;}
        else if (errno != ENOENT) return false;
    }
    const auto temporary = path + ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); out.flush();
    const bool good = out.good(); out.close();
    if (!good || out.fail()) {std::remove(temporary.c_str()); return false;}
    return opennow::storage::ReplaceWithTemporaryFile(temporary, path);
}
void Random(unsigned char* bytes, std::size_t size) {
#ifdef __SWITCH__
    randomGet(bytes, size);
#else
    std::random_device random;
    for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<unsigned char>(random());
#endif
}
struct Key {
    std::array<unsigned char, 32> bytes{};
    ~Key() {mbedtls_platform_zeroize(bytes.data(), bytes.size());}
};
struct Plaintext {
    std::string bytes;
    ~Plaintext() {if (!bytes.empty()) mbedtls_platform_zeroize(bytes.data(), bytes.size());}
};
ReadStatus LoadKey(const std::string& home, Key& key) {
    bool found = false;
    for (const auto suffix : {"", ".bak"}) {
        Plaintext encoded;
        const auto status = Read(home + "/game-sdk-data.key" + suffix, encoded.bytes, 72);
        if (status == ReadStatus::Missing) continue;
        found = true;
        if (status != ReadStatus::Ready || encoded.bytes.size() != 72 || !encoded.bytes.starts_with(kKeyHeader)) continue;
        std::array<unsigned char, 32> checksum{};
        if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(encoded.bytes.data() + 8), 32, checksum.data(), 0) ||
            !std::equal(checksum.begin(), checksum.end(), reinterpret_cast<const unsigned char*>(encoded.bytes.data() + 40))) continue;
        std::copy_n(encoded.bytes.data() + 8, 32, key.bytes.begin());
        return ReadStatus::Ready;
    }
    return found ? ReadStatus::Failed : ReadStatus::Missing;
}
std::string StorePath(const std::string& home, const std::string& scope, const Key& key) {
    const auto* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    std::array<unsigned char, 32> digest{};
    if (!info || mbedtls_md_hmac(info, key.bytes.data(), key.bytes.size(),
        reinterpret_cast<const unsigned char*>(scope.data()), scope.size(), digest.data())) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string name;
    for (const auto c : digest) {name += hex[c >> 4]; name += hex[c & 15];}
    return home + "/game-sdk-hk4e-" + name;
}
std::optional<std::string> Encrypt(const std::string& plain, const Key& key, const std::string& scope) {
    std::array<unsigned char, 12> nonce{}; Random(nonce.data(), nonce.size());
    std::array<unsigned char, 16> tag{};
    std::string cipher(plain.size(), '\0');
    mbedtls_gcm_context ctx; mbedtls_gcm_init(&ctx);
    int code = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key.bytes.data(), 256);
    if (!code) code = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, plain.size(),
        nonce.data(), nonce.size(), reinterpret_cast<const unsigned char*>(scope.data()), scope.size(),
        reinterpret_cast<const unsigned char*>(plain.data()), reinterpret_cast<unsigned char*>(cipher.data()),
        tag.size(), tag.data());
    mbedtls_gcm_free(&ctx);
    if (code) return {};
    return std::string(kHeader) + std::string(reinterpret_cast<char*>(nonce.data()), nonce.size()) +
        std::string(reinterpret_cast<char*>(tag.data()), tag.size()) + cipher;
}
struct Snapshot {Json data = Json::object(); std::uint64_t generation = 0;};
std::optional<Snapshot> Decrypt(const std::string& bytes, const Key& key, const std::string& scope) {
    if (bytes.size() <= 36 || !bytes.starts_with(kHeader)) return {};
    Plaintext plain; plain.bytes.resize(bytes.size() - 36);
    mbedtls_gcm_context ctx; mbedtls_gcm_init(&ctx);
    int code = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key.bytes.data(), 256);
    if (!code) code = mbedtls_gcm_auth_decrypt(&ctx, plain.bytes.size(),
        reinterpret_cast<const unsigned char*>(bytes.data() + 8), 12,
        reinterpret_cast<const unsigned char*>(scope.data()), scope.size(),
        reinterpret_cast<const unsigned char*>(bytes.data() + 20), 16,
        reinterpret_cast<const unsigned char*>(bytes.data() + 36),
        reinterpret_cast<unsigned char*>(plain.bytes.data()));
    mbedtls_gcm_free(&ctx);
    if (code) return {};
    const auto value = ParseSdkJson(plain.bytes);
    if (!value.is_object() || value.value("version", Json()) != 1 ||
        !value.contains("generation") || !value["generation"].is_number_unsigned() ||
        value["generation"].get<std::uint64_t>() == 0 || !value.contains("data")) return {};
    const auto data = GameData(value["data"]);
    if (!data || *data != value["data"]) return {};
    return Snapshot{*data, value["generation"].get<std::uint64_t>()};
}
ReadStatus LoadSnapshot(const std::string& path, const Key& key, const std::string& scope, Snapshot& best) {
    bool found = false;
    // Alternate complete snapshots. Installing one slot can never rotate a
    // corrupt file over the sole surviving good snapshot in the other slot.
    for (const auto suffix : {"-0.vault", "-1.vault", "-0.vault.bak", "-1.vault.bak"}) {
        std::string bytes;
        const auto status = Read(path + suffix, bytes, kMaximum + 36);
        if (status == ReadStatus::Missing) continue;
        found = true;
        if (status != ReadStatus::Ready) continue;
        const auto decoded = Decrypt(bytes, key, scope);
        if (decoded && decoded->generation > best.generation) best = *decoded;
    }
    return best.generation ? ReadStatus::Ready : found ? ReadStatus::Failed : ReadStatus::Missing;
}
} // namespace

struct CloudDataStore::Impl {
    std::string home, scope, account_game_key;
    Event event;
    Json initial = Json::object(), pending = Json::object(), last = Json::object();
    std::mutex mutex;
    std::condition_variable changed;
    std::thread writer;
    bool dirty = false, writing = false, stopping = false, enabled = false, saved_reported = false, failure_reported = false;

    Impl(std::string directory, const std::string& app, const std::string& channel,
         const std::string& account, Event sink, bool serve_restored)
        : home(std::move(directory)), event(std::move(sink)) {
        if (app != "4" || !Digits(channel, 10) || !Digits(account) || home.empty()) return;
        account_game_key="cloud_local_data_"+account;
        scope = "yuanshi-game-sdk-data-v1\nhk4e\n" + app + '\n' + channel + '\n' + account;
        ReadStatus status;
        {
            std::lock_guard lock(disk_mutex);
            Key key; status = LoadKey(home, key);
            if (status == ReadStatus::Ready) {
                Snapshot snapshot;
                const auto path = StorePath(home, scope, key);
                status = path.empty() ? ReadStatus::Failed : LoadSnapshot(path, key, scope, snapshot);
                initial = std::move(snapshot.data);
            }
        }
        enabled = status != ReadStatus::Failed;
        const char* tag = status == ReadStatus::Ready
            ? (serve_restored ? "GAME_SDK_CLOUD_DATA_RESTORED" : "GAME_SDK_CLOUD_DATA_VAULT_READY")
            : (status == ReadStatus::Missing ? "GAME_SDK_CLOUD_DATA_EMPTY" : "GAME_SDK_CLOUD_DATA_RESTORE_FAILED");
        emit(tag, static_cast<int>(initial.size()));
        last = initial;
        if (enabled) writer = std::thread([this] {run();});
    }
    void emit(const char* tag, int code = 0) const {if (event) event(tag, code);}
    bool save(const Json& data) {
        std::lock_guard lock(disk_mutex);
        Key key;
        const auto status = LoadKey(home, key);
        if (status == ReadStatus::Failed) return false;
        if (status == ReadStatus::Missing) {
            Random(key.bytes.data(), key.bytes.size());
            std::array<unsigned char, 32> checksum{};
            if (mbedtls_sha256(key.bytes.data(), key.bytes.size(), checksum.data(), 0)) return false;
            Plaintext encoded{std::string(kKeyHeader) +
                std::string(reinterpret_cast<char*>(key.bytes.data()), key.bytes.size()) +
                std::string(reinterpret_cast<char*>(checksum.data()), checksum.size())};
            if (!Write(home + "/game-sdk-data.key", encoded.bytes)) return false;
            // A single damaged key must not make both data slots unreadable.
            if (!Write(home + "/game-sdk-data.key.bak", encoded.bytes)) return false;
        }
        const auto path = StorePath(home, scope, key);
        if (path.empty()) return false;
        Snapshot previous;
        if (LoadSnapshot(path, key, scope, previous) == ReadStatus::Failed ||
            previous.generation == std::numeric_limits<std::uint64_t>::max()) return false;
        const auto generation = previous.generation + 1;
        Plaintext plain{Json{{"version", 1}, {"generation", generation}, {"data", data}}.dump()};
        if (plain.bytes.size() > kMaximum) return false;
        const auto encoded = Encrypt(plain.bytes, key, scope);
        if (!encoded || !Write(path + (generation % 2 ? "-1.vault" : "-0.vault"), *encoded)) return false;
        // Seed recovery on the very first update, before there is an older
        // generation. Later writes only replace the older slot.
        return previous.generation || Write(path + "-0.vault", *encoded);
    }
    void run() {
        using namespace std::chrono_literals;
        std::unique_lock lock(mutex);
        for (;;) {
            changed.wait(lock, [this] {return dirty || stopping;});
            if (!dirty && stopping) break;
            // Fixed burst window, not a debounce that continuous updates can
            // postpone forever. Shutdown skips it and makes one final attempt.
            const auto due = std::chrono::steady_clock::now() + 500ms;
            changed.wait_until(lock, due, [this] {return stopping;});
            Json data = std::move(pending); dirty = false; writing = true;
            lock.unlock();
            bool ok = false;
            try {ok = save(data);} catch (...) { /* No paths, payloads or exceptions in logs. */ }
            if (ok && !saved_reported) {emit("GAME_SDK_CLOUD_DATA_PERSISTED"); saved_reported = true;}
            if (!ok && !failure_reported) {emit("GAME_SDK_CLOUD_DATA_SAVE_FAILED"); failure_reported = true;}
            lock.lock();
            writing = false;
            if (ok) last = data;
            if (!ok && !dirty && !stopping) {pending = std::move(data); dirty = true;}
            if (stopping && !dirty) break;
            if (!ok && !stopping) changed.wait_for(lock, 5s, [this] {return stopping;});
        }
    }
};

CloudDataStore::CloudDataStore(std::string home, std::string app, std::string channel,
    std::string account, Event event, bool serve_restored) : impl_(std::make_unique<Impl>(
        std::move(home), app, channel, account, std::move(event), serve_restored)) {}
CloudDataStore::~CloudDataStore() {stop();}
const CloudDataStore::Json& CloudDataStore::restored() const {return impl_->initial;}
void CloudDataStore::update(const Json& data) {
    if (!impl_->enabled) return;
    const auto filtered = GameData(data);
    // An unrelated SDK call must not erase the game's existing opaque blob.
    if (!filtered || filtered->empty()) return;
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || (impl_->dirty ? *filtered == impl_->pending : !impl_->writing && *filtered == impl_->last)) return;
    impl_->pending = *filtered; impl_->dirty = true; impl_->changed.notify_one();
}
void CloudDataStore::stop() {
    {std::lock_guard lock(impl_->mutex); impl_->stopping = true; impl_->changed.notify_one();}
    if (impl_->writer.joinable()) impl_->writer.join();
}
bool CloudDataStore::save_session_baseline(const std::string& path) const {
    // Plain C++/JSON only, no credentials: this is the game's own opaque config
    // as read at session start. The caller decides whether to keep it.
    if (path.empty()) return false;
    std::lock_guard lock(disk_mutex);
    try {
        Key key;
        if(LoadKey(impl_->home,key)!=ReadStatus::Ready) return false;
        const auto binding=StorePath(impl_->home,impl_->scope,key);
        if(binding.empty()) return false;
        const auto text = Json{{"version", 2}, {"scope_binding",binding.substr(impl_->home.size()+1)},
            {"data", impl_->initial}}.dump();
        if (text.size() > kMaximum) return false;
        return Write(path, text);
    } catch (...) {return false;}
}
std::vector<CloudDataStore::Json> CloudDataStore::controller_recovery_baselines() const {
    std::vector<Json> result;
    if(impl_->account_game_key.empty() || !impl_->enabled ||
       !impl_->initial.contains(impl_->account_game_key)) return result;
    std::lock_guard lock(disk_mutex);
    Key key;
    if(LoadKey(impl_->home,key)!=ReadStatus::Ready) return result;
    const auto path=StorePath(impl_->home,impl_->scope,key);
    if(path.empty()) return result;
    const auto add=[&](const Json& data) {
        const auto filtered=GameData(data);
        if(!filtered || !filtered->contains(impl_->account_game_key)) return;
        Json selected{{impl_->account_game_key,(*filtered)[impl_->account_game_key]}};
        if(std::find(result.begin(),result.end(),selected)==result.end()) result.push_back(std::move(selected));
    };
    // Existing 147 stores already have authenticated alternating slots and
    // backups. Recover controller fields only after their account+channel AAD
    // verifies, never from an unbound legacy plaintext snapshot.
    std::vector<Snapshot> snapshots;
    for(const auto* suffix:{"-0.vault","-1.vault","-0.vault.bak","-1.vault.bak"}) {
        std::string bytes;
        if(Read(path+suffix,bytes,kMaximum+36)!=ReadStatus::Ready) continue;
        if(auto decoded=Decrypt(bytes,key,impl_->scope)) snapshots.push_back(std::move(*decoded));
    }
    std::sort(snapshots.begin(),snapshots.end(),[](const auto& a,const auto& b){return a.generation>b.generation;});
    for(const auto& snapshot:snapshots) add(snapshot.data);
    for(const auto* suffix:{"", ".bak"}) {
        std::string bytes;
        if(Read(impl_->home+"/game-sdk-hk4e-session-start.json"+suffix,bytes,kMaximum)!=ReadStatus::Ready) continue;
        const auto snapshot=ParseSdkJson(bytes);
        if(!snapshot.is_object() || snapshot.value("version",Json())!=2 || !snapshot.contains("data") ||
           snapshot.value("scope_binding",Json())!=path.substr(impl_->home.size()+1)) continue;
        add(snapshot["data"]);
    }
    return result;
}
} // namespace cloudgenshin::sdk
