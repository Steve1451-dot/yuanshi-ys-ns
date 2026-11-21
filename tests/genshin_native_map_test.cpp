#include "mihoyo/genshin_native_map.hpp"
#include <borealis/extern/tinyxml2/tinyxml2.h>
#include <cassert>
#include <string>

int main() {
    using namespace cloudgenshin::sdk;
    const std::string map=R"(<?xml version="1.0" encoding="utf-16"?><JoystickMap dataVersion="2" hardwareGuid="d74a350e-fe8b-4e9e-bbcd-efff16d34115" hardwareName="XInput Gamepad"><categoryId>0</categoryId><layoutId>0</layoutId><name>Aim+Attack %2B 中文</name><buttonMaps><ActionElementMap><actionId>43</actionId><elementIdentifierId>16</elementIdentifierId></ActionElementMap></buttonMaps><!-- keep+comment --><![CDATA[keep+data]]></JoystickMap>)";
    assert(PrepareNativeMapXml(map).state==NativeMapXmlState::Valid);
    auto legacy=map;
    for(auto& c:legacy) if(c==' ') c='+';
    const auto fixed=PrepareNativeMapXml(legacy);
    assert(fixed.state==NativeMapXmlState::Repaired);
    tinyxml2::XMLDocument doc;
    assert(doc.Parse(fixed.repaired.c_str())==tinyxml2::XML_SUCCESS);
    const auto* root=doc.RootElement();
    assert(std::string(root->Attribute("hardwareGuid"))=="d74a350e-fe8b-4e9e-bbcd-efff16d34115");
    assert(std::string(root->Attribute("hardwareName"))=="XInput Gamepad");
    // Text and comments are not blindly plus/percent decoded; IDs never change.
    assert(std::string(root->FirstChildElement("name")->GetText())=="Aim+Attack+%2B+中文");
    assert(fixed.repaired.find("<!--+keep+comment+-->")!=std::string::npos);
    assert(fixed.repaired.find("<![CDATA[keep+data]]>")!=std::string::npos);
    assert(fixed.repaired.find("<actionId>43</actionId><elementIdentifierId>16</elementIdentifierId>")!=std::string::npos);
    assert(PrepareNativeMapXml(fixed.repaired).state==NativeMapXmlState::Valid);
    const auto schema=PrepareNativeMapXml(R"(<?xml+version="1.0"?><JoystickMap+xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"+xsi:schemaLocation="http://guavaman.com/rewired+http://guavaman.com/schemas/rewired/1.1/JoystickMap.xsd"+custom="literal+attribute"><name>literal+text</name></JoystickMap>)");
    assert(schema.state==NativeMapXmlState::Repaired);
    assert(schema.repaired.find("http://guavaman.com/rewired http://guavaman.com/schemas/rewired/1.1/JoystickMap.xsd")!=std::string::npos);
    assert(schema.repaired.find("custom=\"literal+attribute\"")!=std::string::npos);
    assert(schema.repaired.find("<name>literal+text</name>")!=std::string::npos);
    assert(PrepareNativeMapXml("{\"future\":\"map\"}").state==NativeMapXmlState::Opaque);
    assert(PrepareNativeMapXml("<CustomControllerMap>keep+name</CustomControllerMap>").state==NativeMapXmlState::Opaque);
    assert(PrepareNativeMapXml("<?xml+version=\"1.0\"?><JoystickMap+").state==NativeMapXmlState::Invalid);
    assert(PrepareNativeMapXml("<JoystickMap><broken></JoystickMap>").state==NativeMapXmlState::Invalid);
    assert(PrepareNativeMapXml(std::string(256*1024+1,'x')).state==NativeMapXmlState::Invalid);
}
