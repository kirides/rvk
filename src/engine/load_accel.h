#pragma once

#include <cstddef>
#include <cstdint>

// Loading-time work of the engine thread that can be done cheaper without changing a result (docs/research/streaming.md):
//
//  * Packed-file read buffer (FileSystem2.dll RVA 0x23540, CFFPackedDirectFileStream::Read): every Read of a file inside a
//    volume is a SetFilePointerEx + ReadFile pair, however small (the property streams of the world sectors read a few bytes at
//    a time). A per-stream read-ahead buffer turns most of them into a memcpy. 36 % of the engine thread's time in the loading
//    screens of the savegame was these two system calls.
//  * eCSkydome_PS::CreateAbsorbationImage (Engine.dll 0x6e2cb0): the cloud absorption image is four 32-tap box sums per pixel,
//    each tap a 32-bit `div` for the wrapped coordinate (128 divisions per pixel); recomputed whenever a cloud texture property
//    is set, i.e. on every world / sector load. The loops are replaced by sliding-window sums; the integer sums are exact in
//    float, so the image is bit-identical.
//  * Statistics of the loading work (reads, sounds created, absorption images), rvk.log `engine: stream:` every 5 s.
namespace rvk::engine::loadaccel {

// ---- pure parts (unit-tested) ----

// The engine's loop, transcribed from the disassembly: for every pixel the sums of the blue-channel bytes (offset 2 of the 4-byte
// pixel) over 32 taps to the right / left (x +- k, wrapped with the 32-bit unsigned modulo the engine uses) and 32 taps down / up
// (y +- k), times `scale`, truncated, packed as up<<24 | right<<16 | down<<8 | left. `pitch` in bytes, src and dst different.
void absorbNaive(const std::uint8_t* src, std::uint8_t* dst, std::uint32_t w, std::uint32_t h, std::uint32_t pitch, float scale);
// The same result by sliding windows (O(w * h) instead of 128 * w * h divisions)
void absorbFast(const std::uint8_t* src, std::uint8_t* dst, std::uint32_t w, std::uint32_t h, std::uint32_t pitch, float scale);

// ---- engine hooks ----

// Engine thread, once, before the engine reads its first world: hooks FileSystem2's packed read, patches the absorption loop,
// hooks FMOD's createSound for the statistics. Each piece checks the bytes it patches and stays off when the build differs.
void install();

// Settings (any thread). readCacheKiB: 0 = off, else the read-ahead size per open packed file (4..1024).
void setReadCacheKiB(int kib);
// 0 = the engine's loop, 1 = the sliding-window loop, 2 = both (the engine's loop runs; its image is compared with ours)
void setAbsorbMode(int mode);
// 0 = off, 1 = statistics lines and per-call timing (a clock read around every hooked call), 2 = also compare every buffered read
// with the engine's own read of the same bytes
void setStats(int level);
// debug.sound_compressed: FMOD sounds the game wants decoded inside createSound are created as compressed samples (decoded while playing)
void setSoundCompressed(bool on);

// Engine thread, once per present: the 5 s statistics line
void onPresent();

}  // namespace rvk::engine::loadaccel
