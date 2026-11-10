#pragma once

// Profiling macros for the renderer (Tracy, wolfpld/tracy; CMake option RVK_TRACY, off by default).
//
//   RVK_ZONE("name")            a scope: a Tracy zone named by a string literal (the name must be a literal: Tracy keeps the pointer)
//   RVK_ZONE_DYN(var, name)     the same with a run-time name (a pass's name): var is the zone's variable; Tracy copies the name
//   RVK_FRAME()                 the frame boundary (Tracy's continuous frames; one per Present, after the present's own work)
//   RVK_PLOT("name", value)     a numeric track (value: any arithmetic type)
//   rvk::prof::DynamicZone      a zone as a class member (a scope object's run-time name): begins at construction, ends at destruction
//
// Off (no TRACY_ENABLE): every macro expands to nothing and DynamicZone is empty, so the shipped build is unchanged and the code
// has no Tracy include. Zones stay at pass / stage granularity: never per draw.

#if defined(TRACY_ENABLE)

#include <cstdint>
#include <cstring>

#include <tracy/Tracy.hpp>

namespace rvk::prof {

class DynamicZone {
public:
    explicit DynamicZone(const char* name)
        : zone_(static_cast<std::uint32_t>(__LINE__), __FILE__, sizeof(__FILE__) - 1, __FUNCTION__, sizeof(__FUNCTION__) - 1,
                name != nullptr ? name : "", name != nullptr ? std::strlen(name) : 0, 0, true) {}
    DynamicZone(const DynamicZone&) = delete;
    DynamicZone& operator=(const DynamicZone&) = delete;

private:
    tracy::ScopedZone zone_;
};

}  // namespace rvk::prof

#define RVK_ZONE(name) ZoneScopedN(name)
#define RVK_ZONE_DYN(var, name) ZoneTransientN(var, name, true)
#define RVK_FRAME() FrameMark
#define RVK_PLOT(name, value) TracyPlot(name, value)

#else

namespace rvk::prof {

struct DynamicZone {
    explicit DynamicZone(const char*) {}
};

}  // namespace rvk::prof

#define RVK_ZONE(name) ((void)0)
#define RVK_ZONE_DYN(var, name) ((void)0)
#define RVK_FRAME() ((void)0)
#define RVK_PLOT(name, value) ((void)0)

#endif
