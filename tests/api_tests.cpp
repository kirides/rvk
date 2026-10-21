// Tool API (include/rvk_api.h): BMP encoding, the output-path guard, the screenshot queue.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>

#include "api/capture.h"
#include "rvk_api.h"
#include "test.h"

using namespace rvk;
namespace fs = std::filesystem;

namespace {

std::uint32_t rd32(const std::vector<std::uint8_t>& v, std::size_t at) {
    std::uint32_t x = 0;
    std::memcpy(&x, v.data() + at, 4);
    return x;
}
std::uint16_t rd16(const std::vector<std::uint8_t>& v, std::size_t at) {
    std::uint16_t x = 0;
    std::memcpy(&x, v.data() + at, 2);
    return x;
}

std::vector<std::uint8_t> readFile(const fs::path& f) {
    std::ifstream in(f, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// a fresh directory under %TEMP%
fs::path tempDir(const wchar_t* name) {
    const fs::path d = fs::temp_directory_path() / (std::wstring(L"rvk_tests_") + name + L"_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

}  // namespace

TEST(api_bmp_layout) {
    // 1x2 BGRA, top row (10,20,30,40), bottom row (50,60,70,80)
    const std::vector<std::uint8_t> px{10, 20, 30, 40, 50, 60, 70, 80};
    const auto bmp = api::encodeBmp(px, 1, 2, 24);
    CHECK(bmp.size() == 54 + 2 * 4);  // rows padded from 3 to 4 bytes
    CHECK(bmp[0] == 'B' && bmp[1] == 'M');
    CHECK(rd32(bmp, 2) == bmp.size());
    CHECK(rd32(bmp, 10) == 54);
    CHECK(rd32(bmp, 14) == 40);
    CHECK(rd32(bmp, 18) == 1);
    CHECK(rd32(bmp, 22) == 2);  // positive: bottom-up
    CHECK(rd16(bmp, 26) == 1);
    CHECK(rd16(bmp, 28) == 24);
    CHECK(rd32(bmp, 30) == 0);  // BI_RGB
    CHECK(rd32(bmp, 34) == 8);
    // first stored row = bottom image row, BGR order, alpha dropped, padding zero
    CHECK(bmp[54] == 50 && bmp[55] == 60 && bmp[56] == 70 && bmp[57] == 0);
    CHECK(bmp[58] == 10 && bmp[59] == 20 && bmp[60] == 30 && bmp[61] == 0);

    const auto bmp32 = api::encodeBmp(px, 1, 2, 32);
    CHECK(bmp32.size() == 54 + 8);
    CHECK(rd16(bmp32, 28) == 32);
    CHECK(bmp32[54] == 50 && bmp32[57] == 80 && bmp32[58] == 10 && bmp32[61] == 40);

    // D3DX's layout (measured: 1600x900 X8R8G8B8 -> 5 760 056 bytes)
    const auto dx = api::encodeBmpLikeD3DX(px, 1, 2);
    CHECK(dx.size() == 54 + 8 + 2);
    CHECK(rd32(dx, 2) == dx.size());
    CHECK(rd16(dx, 28) == 32 && rd32(dx, 34) == 0 && rd32(dx, 38) == 2834 && rd32(dx, 42) == 2834);
    CHECK(dx[54] == 50 && dx[57] == 80 && dx[61] == 40 && dx[62] == 0 && dx[63] == 0);
    std::vector<std::uint8_t> big(1600u * 900u * 4u, 0xff);
    CHECK(api::encodeBmpLikeD3DX(big, 1600, 900).size() == 5760056);

    CHECK(api::encodeBmp(px, 2, 2, 24).empty());  // too few pixels
    CHECK(api::encodeBmp(px, 1, 2, 16).empty());
}

TEST(api_extensions) {
    CHECK(api::formatFromExtension(L"C:\\x\\a.png") == api::ImageFormat::Png);
    CHECK(api::formatFromExtension(L"C:\\x\\a.PNG") == api::ImageFormat::Png);
    CHECK(api::formatFromExtension(L"C:\\x\\a.Bmp") == api::ImageFormat::Bmp);
    CHECK(!api::formatFromExtension(L"C:\\x\\a.jpg"));
    CHECK(!api::formatFromExtension(L"C:\\x\\png"));
}

TEST(api_path_within) {
    CHECK(api::pathWithin(L"B:\\Game\\data\\x.png", L"B:\\Game\\data"));
    CHECK(api::pathWithin(L"b:\\GAME\\Data\\sub\\x.png", L"B:\\Game\\data\\"));
    CHECK(api::pathWithin(L"B:\\Game\\data", L"B:\\Game\\data"));
    CHECK(api::pathWithin(L"B:/Game/data/x.png", L"B:\\Game\\data"));
    CHECK(api::pathWithin(L"B:\\Game\\bin\\..\\data\\x.png", L"B:\\Game\\data"));
    CHECK(api::pathWithin(L"\\\\?\\B:\\Game\\data\\x.png", L"B:\\Game\\data"));
    CHECK(!api::pathWithin(L"B:\\Game\\database\\x.png", L"B:\\Game\\data"));
    CHECK(!api::pathWithin(L"B:\\Game\\bin\\x.png", L"B:\\Game\\data"));
    CHECK(!api::pathWithin(L"C:\\Game\\data\\x.png", L"B:\\Game\\data"));
    CHECK(!api::pathWithin(L"B:\\Game", L"B:\\Game\\data"));
}

TEST(api_output_path_guard) {
    const fs::path game = tempDir(L"guard");
    fs::create_directories(game / L"data" / L"common");
    fs::create_directories(game / L"bin");
    CHECK(api::refuseOutputPath(L"shot.png", game).has_value());
    CHECK(api::refuseOutputPath(game / L"bin" / L"shot.jpg", game).has_value());
    CHECK(api::refuseOutputPath(game / L"bin" / L"", game).has_value());
    CHECK(api::refuseOutputPath(game / L"data" / L"shot.png", game).has_value());
    CHECK(api::refuseOutputPath(game / L"DATA" / L"common" / L"new" / L"shot.png", game).has_value());
    CHECK(api::refuseOutputPath(game / L"bin" / L".." / L"data" / L"shot.png", game).has_value());
    CHECK(!api::refuseOutputPath(game / L"bin" / L"spass" / L"screenshots" / L"shot.png", game).has_value());
    CHECK(!api::refuseOutputPath(game / L"database" / L"shot.bmp", game).has_value());
    CHECK(!api::refuseDataPath(game / L"bin" / L"x.bmp", game).has_value());
    CHECK(api::refuseDataPath(game / L"data" / L"x.bmp", game).has_value());
    CHECK(!api::refuseOutputPath(game / L"data" / L"x.png", fs::path()).has_value());  // no game dir: no data guard
    std::error_code ec;
    fs::remove_all(game, ec);
}

TEST(api_capture_queue) {
    const fs::path dir = tempDir(L"queue");
    api::CaptureQueue q;
    std::string reason;
    const std::uint32_t t0 = q.request(dir / L"now.png", api::ImageFormat::Png, 0, &reason);
    const std::uint32_t t2 = q.request(dir / L"sub" / L"later.bmp", api::ImageFormat::Bmp, 2, &reason);
    CHECK(t0 > 0 && t2 > t0);
    CHECK(q.pending() == 2);
    std::string msg;
    CHECK(q.status(t2, &msg) == RVK_SHOT_PENDING);
    CHECK(msg == "waiting 2 presents");
    CHECK(q.status(9999, &msg) == RVK_SHOT_FAILED);

    int reads = 0;
    // 2x1 image: red, green (BGRA)
    const auto read = [&](std::vector<std::uint8_t>& bgra, std::uint32_t& w, std::uint32_t& h) {
        ++reads;
        bgra = {0, 0, 255, 255, 0, 255, 0, 255};
        w = 2;
        h = 1;
        return true;
    };
    q.onPresent(1, read);  // t0 due
    CHECK(reads == 1);
    q.onPresent(2, read);  // t2 waits
    CHECK(reads == 1);
    q.onPresent(3, read);  // t2 due: 2 presents skipped (wait 0 = the next present)
    CHECK(reads == 2);
    q.onPresent(4, read);  // nothing pending: no read-back
    CHECK(reads == 2);
    CHECK(q.waitIdle(std::chrono::seconds(10)));
    CHECK(q.pending() == 0);
    CHECK(q.status(t0, &msg) == RVK_SHOT_DONE);
    CHECK_MSG(msg == "frame 1, waited 0 presents, 2x1", msg);
    CHECK(q.status(t2, &msg) == RVK_SHOT_DONE);
    CHECK_MSG(msg == "frame 3, waited 2 presents, 2x1", msg);
    const auto png = readFile(dir / L"now.png");
    CHECK(png.size() > 8 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
    const auto bmp = readFile(dir / L"sub" / L"later.bmp");
    CHECK(bmp.size() == 54 + 8);
    CHECK(bmp.size() == 62 && bmp[54] == 0 && bmp[55] == 0 && bmp[56] == 255 && bmp[57] == 0 && bmp[58] == 255);
    CHECK(!fs::exists(dir / L"now.png.tmp"));

    // read-back failure, shutdown of the renderer
    const std::uint32_t tf = q.request(dir / L"fail.png", api::ImageFormat::Png, 0, &reason);
    q.onPresent(5, [](std::vector<std::uint8_t>&, std::uint32_t&, std::uint32_t&) { return false; });
    CHECK(q.status(tf, &msg) == RVK_SHOT_FAILED);
    const std::uint32_t tw = q.request(dir / L"never.png", api::ImageFormat::Png, 100, &reason);
    q.failWaiting("gone");
    CHECK(q.status(tw, &msg) == RVK_SHOT_FAILED && msg == "gone");

    // queue limit
    std::uint32_t accepted = 0;
    for (std::size_t i = 0; i < api::CaptureQueue::kMaxPending + 5; ++i) {
        if (q.request(dir / L"x.png", api::ImageFormat::Png, 1000, &reason) != 0) ++accepted;
    }
    CHECK(accepted == api::CaptureQueue::kMaxPending);
    q.failWaiting("done");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(api_stats_layout) {
    CHECK(sizeof(struct rvk_stats) == 56);
    CHECK(offsetof(struct rvk_stats, frame_index) == 8);
    CHECK(offsetof(struct rvk_stats, fps) == 44);
    CHECK(offsetof(struct rvk_stats, texture_cpu_bytes) == 48);
}
