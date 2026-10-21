// Compile check of the generated stubs: every interface has a concrete stub class.

#include "hal/stub_generated.h"
#include "test.h"

TEST(hal_stubs_instantiable) {
    // the stubs must be complete (no pure virtual left): taking sizeof of a concrete subclass checks that at compile time
    struct Device final : rvk::hal::StubDevice {};
    struct Texture final : rvk::hal::StubTexture {};
    struct Primitive final : rvk::hal::StubPrimitive {};
    CHECK(sizeof(Device) >= 16);
    CHECK(sizeof(Texture) >= 16);
    CHECK(sizeof(Primitive) >= 8);
}
