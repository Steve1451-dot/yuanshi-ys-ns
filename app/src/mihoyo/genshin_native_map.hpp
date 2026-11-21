#pragma once
#include <string>
#include <string_view>

namespace cloudgenshin::sdk {
enum class NativeMapXmlState {Opaque,Valid,Repaired,Invalid};
struct NativeMapXml {
    NativeMapXmlState state=NativeMapXmlState::Opaque;
    std::string repaired;
};
// Runtime reply only. Never rewrites the encrypted original, action IDs,
// element IDs, controller GUIDs or the user's button choices.
NativeMapXml PrepareNativeMapXml(std::string_view value);
}
