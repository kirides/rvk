#pragma once

// Perf windows (docs/research/performance.md 33): repeated, tagged GPU / CPU measurement windows inside one game session, for in-run A/B
// (scripts/perf-ab.ps1). The overlay command `perf window <frames> <tag>` arms a window; from the next recorded frame on, `frames` frames
// have their GPU passes timed (as -PerfFrame does for one fixed frame) and the HAL closes its 5 s stats interval right before the window's
// first and right after its last frame, so the `perf:` CPU line, the `native: stages (5 s)` line and every other interval report of that
// interval cover exactly the window. Between the markers
//   perf: window <tag> begin at frame F (N frames)
//   perf: window <tag> end at frame F
// the interval's reports are logged; when the GPU results of the window's frames are back:
//   perf: window <tag> gpu: N frames timed, main mean M ms sd S, uploads mean U ms
//   perf: window <tag> pass mean M ms sd S n K draws D | <key>
// <key> = the pass label without its running number and draw count ("1600x900, 1 colour (100), depth 130"), summed per frame over the
// passes with that key, mean / sd over the timed frames (a key missing in a frame counts 0 there), D = the mean draws per frame.
// Pure bookkeeping: callers are the overlay (arm), d3d9::Device (beginFrame / gpuFrame) and the HAL (afterPresent). Thread-safe.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rvk::perfwin {

struct Segment {
    std::string_view label;
    double ms = 0.0;
};

// arms a window of `frames` frames (1 .. 100000) tagged `tag` (no spaces); false + why when one is already armed / running
bool arm(std::uint32_t frames, std::string_view tag, std::string* why = nullptr);

// d3d9::Device, when it starts recording frame `frame`: true = time this frame's GPU passes in detail (a window's frame)
bool beginFrame(std::uint64_t frame);

// the HAL after presenting frame `presented`: Begin = the next frame is a window's first (close the stats interval now), End = the window's
// last frame was presented (close the interval now). `tag` gets the window's tag for the marker lines.
enum class Edge { None, Begin, End };
Edge afterPresent(std::uint64_t presented, std::string* tag);

// d3d9::Device when the GPU results of a timed frame come back; false = not a window frame (the caller logs it as before). Logs the
// window's GPU summary once every frame of it came back (or 16 frames after its end at the latest).
bool gpuFrame(std::uint64_t frame, double mainMs, double uploadMs, const std::vector<Segment>& segments);

// the aggregation key of a pass label: "pass 12: 1600x900, 1 colour (97), depth 130, 634 draws" -> "1600x900, 1 colour (97), depth 130"
// (+ draws = 634); "barriers / copies / inline uploads before pass 3" -> "barriers / copies / inline uploads"; other labels unchanged
std::string passKey(std::string_view label, std::uint32_t* draws = nullptr);

// a window is recording (its first frame started, its last not presented yet)
bool running();

// (tests) forget every window
void reset();

}  // namespace rvk::perfwin
