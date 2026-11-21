#pragma once
#include <cstdint>
#include <string_view>

namespace cloudgenshin::input {
// Match the Android holder's change-driven idle input. Repeated active/held
// reports are deliberately preserved for the established weak-network path.
inline bool SuppressIdleGamepadReports(std::string_view app,unsigned terminal) {
    return app=="4" && terminal==3;
}
class IdleGamepadPolicy {
public:
    bool needs_report(bool active,std::uint64_t epoch,bool force=false) const {
        return force || active || !neutral_sent_ || epoch!=epoch_;
    }
    // Call only after the transport accepted the report, never on send failure.
    void delivered(bool active,std::uint64_t epoch) {neutral_sent_=!active;epoch_=epoch;}
    void invalidate() {neutral_sent_=false;}
private:
    bool neutral_sent_=false;
    std::uint64_t epoch_=0;
};
} // namespace cloudgenshin::input
