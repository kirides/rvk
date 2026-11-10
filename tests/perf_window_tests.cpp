// Perf windows (src/core/perf_window.*, docs/research/performance.md 33): the frame bookkeeping the HAL / device / overlay drive.

#include <string>
#include <vector>

#include "core/perf_window.h"
#include "test.h"

using namespace rvk;

TEST(perf_window_pass_key) {
    std::uint32_t d = 99;
    CHECK(perfwin::passKey("pass 12: 1600x900, 1 colour (97), depth 130, 634 draws", &d) == "1600x900, 1 colour (97), depth 130");
    CHECK(d == 634);
    CHECK(perfwin::passKey("barriers / copies / inline uploads before pass 3", &d) == "barriers / copies / inline uploads");
    CHECK(d == 0);
    CHECK(perfwin::passKey("pass water.rt at AfterRefractionCopy", &d) == "pass water.rt at AfterRefractionCopy");
    CHECK(perfwin::passKey("present blit + barriers since the last pass") == "present blit + barriers since the last pass");
}

TEST(perf_window_frames) {
    perfwin::reset();
    std::string tag;
    CHECK(!perfwin::arm(0, "x"));
    CHECK(!perfwin::arm(10, "a b"));
    // the HAL's order per present P: (overlay command) -> device begins frame P + 1 -> afterPresent(P)
    CHECK(perfwin::arm(3, "A1"));
    CHECK(!perfwin::arm(3, "A2"));  // one at a time
    CHECK(perfwin::beginFrame(101));  // armed at present 100: the window starts with the next recorded frame (101)
    CHECK(perfwin::afterPresent(100, &tag) == perfwin::Edge::Begin && tag == "A1");
    CHECK(perfwin::running());
    CHECK(perfwin::beginFrame(102));
    CHECK(perfwin::afterPresent(101, &tag) == perfwin::Edge::None);
    CHECK(perfwin::beginFrame(103));
    CHECK(perfwin::afterPresent(102, &tag) == perfwin::Edge::None);
    CHECK(!perfwin::beginFrame(104));         // past the window
    CHECK(perfwin::afterPresent(103, &tag) == perfwin::Edge::End && tag == "A1");
    CHECK(!perfwin::running());
    // the GPU results come back later: window frames are taken, others are not
    const std::vector<perfwin::Segment> segs = {{"pass 1: 1600x900, 1 colour (100), depth 130, 500 draws", 0.5},
                                                {"pass 2: 1600x900, 1 colour (100), depth 130, 0 draws", 0.01}};
    CHECK(perfwin::gpuFrame(101, 5.0, 0.1, segs));
    CHECK(perfwin::gpuFrame(102, 5.0, 0.1, segs));
    CHECK(!perfwin::gpuFrame(104, 5.0, 0.1, segs));
    CHECK(perfwin::gpuFrame(103, 5.0, 0.1, segs));  // the last: summarised
    CHECK(!perfwin::gpuFrame(103, 5.0, 0.1, segs));  // (gone)
    // a second window may follow at once
    CHECK(perfwin::arm(1, "B1"));
    CHECK(perfwin::beginFrame(105));
    CHECK(perfwin::afterPresent(104, &tag) == perfwin::Edge::Begin && tag == "B1");
    CHECK(perfwin::afterPresent(105, &tag) == perfwin::Edge::End && tag == "B1");
    // results that never come: summarised 16 presents after the end
    CHECK(perfwin::afterPresent(130, &tag) == perfwin::Edge::None);
    CHECK(!perfwin::gpuFrame(105, 5.0, 0.1, segs));
    perfwin::reset();
}
