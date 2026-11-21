#include "mihoyo/input_transport_policy.hpp"
#include <cassert>
int main() {
    using namespace cloudgenshin::rtc;
    // Follow the single source of truth: the allowed Xbox identity changed in
    // 0.2.131, so a literal 0x028e here would silently test the old pad.
    assert(PreferNativeGamepadRtc("4",cloudgenshin::input::kXboxVendorId,cloudgenshin::input::kXboxProductId,false));
    assert(!PreferNativeGamepadRtc("4",cloudgenshin::input::kXboxVendorId,cloudgenshin::input::kXboxProductId,true));
    assert(!PreferNativeGamepadRtc("8",cloudgenshin::input::kXboxVendorId,cloudgenshin::input::kXboxProductId,false));
    assert(!PreferNativeGamepadRtc("4",0x054c,0x0ce6,false));
    assert(SelectInputTransport(true,true,true)==InputTransport::Rtc);
    assert(SelectInputTransport(true,true,false)==InputTransport::WebControl);
    assert(SelectInputTransport(true,false,true)==InputTransport::WebControl);
    assert(SelectInputTransport(false,true,false)==InputTransport::Rtc);
    assert(SelectInputTransport(false,false,true)==InputTransport::Unavailable);
    InputTransportEpoch epoch;
    assert(epoch.observe(InputTransport::WebControl) && epoch.generation()==1);
    assert(!epoch.observe(InputTransport::WebControl) && epoch.generation()==1);
    assert(epoch.observe(InputTransport::Rtc) && epoch.generation()==2);
    assert(RedeclareInput(true,true,1,2,false)); // Ready remains true across route change.
    assert(!RedeclareInput(true,true,2,2,false));
    assert(RedeclareInput(true,true,2,2,true)); // Launcher pointer -> game entry.
    assert(!RedeclareInput(false,true,1,2,true));
    assert(epoch.observe(InputTransport::Unavailable));
    assert(epoch.observe(InputTransport::WebControl) && epoch.generation()==4);
    epoch.reset();assert(epoch.kind()==InputTransport::Unavailable && epoch.generation()==0);
}
