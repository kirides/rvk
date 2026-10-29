// Frame generation (src/vk/framegen.*, the framegen.* settings; docs/research/frame-generation.md): provider selection from the
// FFX API's version list, the capability requirements of framegen.mode and its fsr4 option.

#include <string>
#include <vector>

#include "core/caps.h"
#include "core/settings.h"
#include "test.h"
#include "vk/framegen.h"

using namespace rvk;
using caps::Cap;

namespace {

// what the 7900 XTX reported (frame-generation.md 1.2): frame generation 3.1.6; swapchain 4.0.0 * (driver) and 3.1.7
std::vector<vk::FrameGenVersion> fgList() { return {vk::parseFrameGenVersion(0xf600000000c01006ull, "3.1.6")}; }
std::vector<vk::FrameGenVersion> swapchainList() {
    return {vk::parseFrameGenVersion(0xf65d564b81000000ull, "4.0.0 *"), vk::parseFrameGenVersion(0xf65d564b00c01007ull, "3.1.7")};
}

caps::Report reportWith(bool fsr3, bool ml) {
    caps::Report r;
    r.known = true;
    r.gpu = "Test GPU";
    r[Cap::FrameGeneration] = {true, fsr3, fsr3 ? "" : "no provider", ""};
    r[Cap::FrameGenerationMl] = {true, ml, ml ? "" : "the driver offers no FSR 4 frame generation", ""};
    return r;
}

}  // namespace

TEST(framegen_version_parsing) {
    const auto a = vk::parseFrameGenVersion(0xf600000000c01006ull, "3.1.6");
    CHECK(a.major == 3 && a.minor == 1 && a.patch == 6 && !a.fromDriver);
    const auto b = vk::parseFrameGenVersion(0xf65d564b81000000ull, "4.0.0 *");
    CHECK(b.major == 4 && b.minor == 0 && b.patch == 0 && b.fromDriver);  // a trailing " *" and bit 31 of the id both mark a driver version
    const auto c = vk::parseFrameGenVersion(0x80000001ull, "4.0.2");
    CHECK(c.fromDriver);  // by the id's bit 31 alone
    const auto d = vk::parseFrameGenVersion(7, "FSR 4.0.2 (ml)");
    CHECK(d.major == 4 && d.minor == 0 && d.patch == 2 && !d.fromDriver);  // the first digits of the name
    const auto e = vk::parseFrameGenVersion(1, "");
    CHECK(e.major == 0 && !e.fromDriver);
}

TEST(framegen_provider_pick) {
    const auto list = fgList();
    const vk::FrameGenVersion* fsr3 = vk::pickFrameGenProvider(list, false);
    CHECK(fsr3 != nullptr && fsr3->name == "3.1.6" && fsr3->id == 0xf600000000c01006ull);
    CHECK(vk::pickFrameGenProvider(list, true) == nullptr);  // RDNA 3: no ML provider offered, nothing is guessed

    // a GPU that offers both: the newest of each family
    std::vector<vk::FrameGenVersion> both = {vk::parseFrameGenVersion(1, "3.1.4"), vk::parseFrameGenVersion(2, "4.0.2"),
                                             vk::parseFrameGenVersion(3, "3.1.6"), vk::parseFrameGenVersion(4, "4.0.1")};
    CHECK(vk::pickFrameGenProvider(both, false)->name == "3.1.6");
    CHECK(vk::pickFrameGenProvider(both, true)->name == "4.0.2");
    CHECK(vk::pickFrameGenProvider({}, false) == nullptr);
    CHECK(vk::describeVersions(list) == "0xf600000000c01006: 3.1.6");
}

TEST(framegen_swapchain_pin_never_the_driver_version) {
    // the driver-provided 4.0.0 is the API's default and crashes at destroy (frame-generation.md 5.3): the pin is the DLL's 3.1.7
    const auto list = swapchainList();
    const vk::FrameGenVersion* p = vk::pickSwapchainProvider(list);
    CHECK(p != nullptr && p->name == "3.1.7" && !p->fromDriver);
    // only the driver's: nothing usable (rather than the crashing one)
    CHECK(vk::pickSwapchainProvider({list[0]}) == nullptr);
    CHECK(vk::pickSwapchainProvider({}) == nullptr);
    // a newer version of the DLL itself wins
    std::vector<vk::FrameGenVersion> more = list;
    more.push_back(vk::parseFrameGenVersion(9, "3.2.0"));
    CHECK(vk::pickSwapchainProvider(more)->name == "3.2.0");
}

TEST(framegen_settings_and_capabilities) {
    settings::Registry reg;
    const settings::RendererSettings rs = settings::registerRendererSettings(reg);
    CHECK(rs.framegenMode != nullptr && rs.framegenManual != nullptr && rs.framegenDebug != nullptr);
    CHECK(rs.framegenMode->spec().defaultValue == 0 && rs.framegenMode->asEnum() == "off");
    CHECK(rs.framegenMode->spec().requirements == caps::bit(Cap::FrameGeneration));
    CHECK(rs.framegenMode->spec().optionRequirements.size() == 3 && rs.framegenMode->spec().optionRequirements[2] == caps::bit(Cap::FrameGenerationMl));
    CHECK(!rs.framegenDebug->abEligible() && rs.framegenManual->abEligible());
    // developer settings until the presentation (F3) exists: in debug mode only
    reg.set(*rs.debugMode, 1.0);
    CHECK(reg.setText("framegen.mode", "fsr4") == settings::Result::Ok);
    CHECK(reg.setText("framegen.manual", "true") == settings::Result::Ok);

    // no device yet: unavailable, the request is kept
    CHECK(!rs.framegenMode->available());
    CHECK(rs.framegenMode->asEnum() == "off" && !rs.framegenManual->asBool());
    // RDNA 3: FSR 3 only. fsr4 is greyed (not an available option) and applies as fsr3
    reg.setCapabilities(reportWith(true, false));
    CHECK(rs.framegenMode->available());
    CHECK(rs.framegenMode->optionAvailable(0) && rs.framegenMode->optionAvailable(1) && !rs.framegenMode->optionAvailable(2));
    CHECK(rs.framegenMode->value() == 2.0);  // the player's choice is kept
    CHECK(rs.framegenMode->asEnum() == "fsr3");
    CHECK(reg.optionUnavailableReason(*rs.framegenMode, 2).find("FSR 4") != std::string::npos);
    CHECK(reg.optionUnavailableReason(*rs.framegenMode, 1).empty());
    CHECK(rs.framegenManual->asBool());
    // RDNA 4: both
    reg.setCapabilities(reportWith(true, true));
    CHECK(rs.framegenMode->asEnum() == "fsr4");
    // no provider / no DLL: everything is off, whatever was asked
    reg.setCapabilities(reportWith(false, false));
    CHECK(!rs.framegenMode->available());
    CHECK(rs.framegenMode->asEnum() == "off" && !rs.framegenManual->asBool());
    CHECK(reg.toToml().find("mode = \"fsr4\"") != std::string::npos);  // the file keeps the request
}

TEST(framegen_caps_names) {
    CHECK(caps::name(Cap::FrameGeneration) == "frame_generation");
    CHECK(caps::name(Cap::FrameGenerationMl) == "frame_generation_ml");
    CHECK(caps::name(Cap::FrameGenerationQueues) == "frame_generation_queues");
    CHECK(caps::kCount <= 32);  // Mask is 32 bits
}
