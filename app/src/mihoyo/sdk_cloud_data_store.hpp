#pragma once
#include <borealis/extern/nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cloudgenshin::sdk {
// Host storage for the game's own cloud_local_data_* API, NOT a controller
// remapping table, login store or cloud-machine memory snapshot. Only hk4e is
// enabled until the other games' storage contracts have been verified.
class CloudDataStore {
public:
    using Json = nlohmann::json;
    using Event = std::function<void(const char*, int)>;
    // serve_restored=false keeps the vault only as a merge base for saving;
    // the bridge is told not to return it to the game.
    CloudDataStore(std::string home, std::string app_id, std::string channel_id,
                   std::string account_id, Event event = {}, bool serve_restored = true);
    ~CloudDataStore();
    CloudDataStore(const CloudDataStore&) = delete;
    CloudDataStore& operator=(const CloudDataStore&) = delete;
    // Read before starting the peer. No filesystem work in SDK/media callbacks.
    const Json& restored() const;
    // Latest-only bounded queue. The dedicated writer coalesces bursts.
    void update(const Json& data);
    // Flush the latest pending update and join after stopping the peer.
    void stop();
    // Copy of the vault as it existed at session start, before the game could
    // overwrite it with defaults. Written once when the caller asks; separate
    // from the encrypted slots so a bad session can never destroy the baseline.
    bool save_session_baseline(const std::string& path) const;
    // Read before replacing the session baseline. Returns only this account's
    // existing game key from bounded local backups, never login data or paths
    // directed by a server. Controller validation is owned by the SDK bridge.
    std::vector<Json> controller_recovery_baselines() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace cloudgenshin::sdk
