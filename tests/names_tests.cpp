// Debug object names (vk/debug_names.h): the texture name scheme (pure string building, no Vulkan device).

#include <string>

#include "test.h"
#include "vk/debug_names.h"

using namespace rvk;

namespace {

vk::names::TextureInfo base() {
    vk::names::TextureInfo t;
    t.id = 17;
    t.width = 512;
    t.height = 256;
    t.format = "DXT1";
    return t;
}

}  // namespace

TEST(names_plain_texture) {
    vk::names::TextureInfo t = base();
    CHECK(vk::names::textureName(t) == "tex 17 512x256 DXT1");
    t.levels = 10;
    t.slot = 57;
    CHECK(vk::names::textureName(t) == "tex 17 512x256 DXT1 L10 [slot 57]");
}

TEST(names_role_label_and_index) {
    vk::names::TextureInfo t = base();
    t.format = "DF24";
    t.renderTarget = true;
    t.label = "cascade shadow map";
    t.labelIndex = 2;
    t.slot = 9;
    CHECK(vk::names::textureName(t) == "cascade shadow map #2 | rt 17 512x256 DF24 [slot 9]");
    t.labelIndex = ~0u;
    t.label = "depth map";
    CHECK(vk::names::textureName(t) == "depth map | rt 17 512x256 DF24 [slot 9]");
}

TEST(names_cube_volume_dynamic) {
    vk::names::TextureInfo t = base();
    t.cube = true;
    t.dynamic = true;
    CHECK(vk::names::textureName(t) == "tex 17 512x256 DXT1 cube dynamic");
    t = base();
    t.volume = true;
    t.depth = 32;
    CHECK(vk::names::textureName(t) == "tex 17 512x256x32 DXT1 vol");
}

TEST(names_enabled_flag) {
    CHECK(vk::names::enabled());  // default on
    vk::names::setEnabled(false);
    CHECK(!vk::names::enabled());
    vk::names::setEnabled(true);
    CHECK(vk::names::enabled());
}
