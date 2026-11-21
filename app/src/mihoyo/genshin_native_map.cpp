#include "mihoyo/genshin_native_map.hpp"
#include <borealis/extern/tinyxml2/tinyxml2.h>

namespace cloudgenshin::sdk {
namespace {
bool JoystickXml(std::string_view value) {
    tinyxml2::XMLDocument document;
    if(document.Parse(value.data(),value.size())!=tinyxml2::XML_SUCCESS) return false;
    const auto* root=document.RootElement();
    return root && std::string_view(root->Name())=="JoystickMap";
}
}
NativeMapXml PrepareNativeMapXml(std::string_view value) {
    constexpr std::size_t kMaximumXmlBytes=256*1024;
    // Other serializations/unknown controller families retain their existing
    // opaque contract. Only recognise the real Rewired JoystickMap envelope.
    if(value.size()>kMaximumXmlBytes) return {NativeMapXmlState::Invalid,{}};
    if(!(value.starts_with("<?xml") || value.starts_with("<JoystickMap")) ||
       value.find("<JoystickMap")==std::string_view::npos) return {};
    if(JoystickXml(value)) return {NativeMapXmlState::Valid,{}};
    // Legacy snapshots contain form-encoded spaces in XML markup, e.g.
    // <?xml+version=...?><JoystickMap+dataVersion=...>. URI re-encoding then
    // protected those '+' bytes as %2B. Do not globally URL-decode XML: a
    // literal '+' or '%2B' in the user's map name/text must survive.
    if(!(value.starts_with("<?xml+version=") || value.starts_with("<JoystickMap+")))
        return {NativeMapXmlState::Invalid,{}};
    std::string candidate(value);
    bool tag=false;
    char quote=0;
    for(std::size_t i=0;i<candidate.size();++i) {
        if(!tag && (value.substr(i).starts_with("<!--") || value.substr(i).starts_with("<![CDATA["))) {
            const bool comment=value.substr(i).starts_with("<!--");
            const auto end=value.find(comment?"-->":"]]>",i+(comment?4:9));
            if(end==std::string_view::npos) return {NativeMapXmlState::Invalid,{}};
            i=end+2;
            continue;
        }
        auto& c=candidate[i];
        if(!tag) {if(c=='<') tag=true;continue;}
        if(quote) {if(c==quote) quote=0;continue;}
        if(c=='\'' || c=='"') quote=c;
        else if(c=='>') tag=false;
        else if(c=='+') c=' ';
    }
    // This known device label was damaged by the same legacy encoding. Do
    // not change arbitrary attribute values, names, percent escapes or GUIDs.
    constexpr std::string_view broken="hardwareName=\"XInput+Gamepad\"";
    if(const auto at=candidate.find(broken);at!=std::string::npos)
        candidate[at+std::string_view("hardwareName=\"XInput").size()]=' ';
    constexpr std::string_view schema="xsi:schemaLocation=\"http://guavaman.com/rewired+http://guavaman.com/schemas/rewired/1.1/JoystickMap.xsd\"";
    if(const auto at=candidate.find(schema);at!=std::string::npos)
        candidate[at+std::string_view("xsi:schemaLocation=\"http://guavaman.com/rewired").size()]=' ';
    if(!JoystickXml(candidate)) return {NativeMapXmlState::Invalid,{}};
    return {NativeMapXmlState::Repaired,std::move(candidate)};
}
}
