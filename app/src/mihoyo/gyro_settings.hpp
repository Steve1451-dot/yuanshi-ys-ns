#pragma once
#include "mihoyo/input_packet.hpp"
#include "atomic_file_replace.hpp"
#include <fstream>
#include <string>
#include <string_view>

namespace cloudgenshin::input {
// Aim assistance only. No button targets, action names or local remapping mode.
struct GyroSettings {
    bool gyro_assist = true;
    unsigned gyro_sensitivity = 100;
    bool gyro_aim_only = true, gyro_invert_y = false, gyro_invert_x = true;
    bool operator==(const GyroSettings&) const = default;
};
inline bool ValidGyroSettings(const GyroSettings& value) {
    return value.gyro_sensitivity == 50 || value.gyro_sensitivity == 100 ||
        value.gyro_sensitivity == 150 || value.gyro_sensitivity == 200;
}
inline GyroSettings LoadGyroSettings(const std::string& home) {
    for (const auto suffix : {"", ".bak"}) {
        std::ifstream in(home + "/genshin-gyro.txt" + suffix, std::ios::binary);
        if (!in) continue;
        in.seekg(0, std::ios::end);
        if (in.tellg() < 0 || in.tellg() > 128) continue;
        in.seekg(0);
        std::string magic; int version = 0, enabled = -1, aim = -1, y = -1, x = -1;
        GyroSettings value;
        if (!(in >> magic >> version >> enabled >> value.gyro_sensitivity >> aim >> y >> x) ||
            magic != "YUANSHI-GYRO" || version != 1 || enabled < 0 || enabled > 1 ||
            aim < 0 || aim > 1 || y < 0 || y > 1 || x < 0 || x > 1 || !ValidGyroSettings(value)) continue;
        in >> std::ws;
        if (!in.eof()) continue;
        value.gyro_assist = enabled; value.gyro_aim_only = aim;
        value.gyro_invert_y = y; value.gyro_invert_x = x;
        return value;
    }
    return {};
}
inline bool SaveGyroSettings(const std::string& home, const GyroSettings& value) {
    if (!ValidGyroSettings(value)) return false;
    const auto path = home + "/genshin-gyro.txt";
    std::ofstream out(path + ".tmp", std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "YUANSHI-GYRO 1 " << value.gyro_assist << ' ' << value.gyro_sensitivity << ' '
        << value.gyro_aim_only << ' ' << value.gyro_invert_y << ' ' << value.gyro_invert_x << '\n';
    out.flush(); const bool good = out.good(); out.close();
    return good && !out.fail() && opennow::storage::ReplaceWithTemporaryFile(path + ".tmp", path);
}
} // namespace cloudgenshin::input
