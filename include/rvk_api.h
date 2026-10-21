/*
 * rvk_api.h: the tool API of risen-enhanced-graphics' d3d9.dll (the Vulkan renderer for Risen Enhanced, x64).
 *
 * Self-contained C header: copy it into tools that talk to the renderer (risen-spass). Design and rationale:
 * docs/research/reference-compare.md section 4.1 of the risen-enhanced-graphics repo.
 *
 * Every export is optional for callers: resolve with GetProcAddress(GetModuleHandleW(L"d3d9.dll"), "rvk_...") and treat a
 * missing symbol as "not our renderer". All functions are callable from any thread at any time after the DLL is loaded
 * (also before the graphics device exists); none of them blocks on the renderer.
 *
 * Modes: the DLL also runs as a plain D3D9 forwarder (config mode "passthrough") or a tracing wrapper ("trace"). There the
 * game's D3D9 renderer draws, rvk_capabilities() returns 0 and rvk_screenshot() refuses every request: fall back to a D3D9
 * capture path. A Vulkan session can also fall back to passthrough at startup (no usable Vulkan device), before the window.
 */
#ifndef RVK_API_H
#define RVK_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* rvk_capabilities() bits: what the active renderer supports right now (0 outside Vulkan mode) */
#define RVK_CAP_SCREENSHOT 0x1u      /* rvk_screenshot / rvk_screenshot_status */
#define RVK_CAP_STABLE_CAPTURE 0x2u  /* RVK_SHOT_STABLE (not implemented yet: never set) */
#define RVK_CAP_STATS 0x4u           /* rvk_stats has live renderer numbers */
#define RVK_CAP_OVERLAY 0x8u         /* the in-game overlay exists (rvk_overlay) */

/* rvk_screenshot() flags: none defined yet. Unknown bits make the request fail (returns 0), so a caller can probe for a
   future flag by trying it. Reserved: 0x1 RVK_SHOT_STABLE, 0x2 RVK_SHOT_DUMP_RTS (reference-compare.md 4.2). */

/* rvk_screenshot_status() results */
#define RVK_SHOT_PENDING 0
#define RVK_SHOT_DONE 1
#define RVK_SHOT_FAILED (-1)

/* rvk_stats::mode */
#define RVK_MODE_PASSTHROUGH 0
#define RVK_MODE_TRACE 1
#define RVK_MODE_VULKAN 2

/* Versioned by size: new fields are only ever appended. Pass sizeof(struct rvk_stats) of your copy of this header; the DLL
   writes min(size, its own size) bytes and returns that count (in `size` too). Fields beyond the returned size are untouched. */
struct rvk_stats {
    uint32_t size;                /* bytes written by rvk_stats */
    uint32_t mode;                /* RVK_MODE_* */
    uint64_t frame_index;         /* presents so far (= rvk_frame_index) */
    uint32_t draws_last_frame;    /* draws recorded in the last presented frame */
    uint32_t unimplemented_slots; /* distinct engine HAL slots called that the Vulkan HAL does not implement */
    uint64_t draws_total;         /* draws recorded since the device was created */
    uint64_t draws_skipped_total; /* draws dropped since creation (no shader, no target, vertex input, pipeline) */
    uint32_t screenshots_pending; /* rvk_screenshot requests not finished yet (waiting, capturing or encoding) */
    float fps;                    /* presents per second, averaged over the last second (0 before that) */
    uint64_t texture_cpu_bytes;   /* CPU-side copies of textures (lockable / not yet uploaded) */
};                                /* version 1: 56 bytes */

/* "0.1.0": lets tools detect the renderer DLL. */
const char* rvk_version(void);

/* RVK_CAP_* bits of the active renderer. */
uint32_t rvk_capabilities(void);

/* Queues a capture of the finished back buffer (scene + post-processing + GUI, the image about to be presented) of the
   present `wait_frames` presents from now: 0 = the next present (the frame the game is building when called from its main
   thread between two presents), n = the n-th present after that.
   path: absolute, UTF-16, ending in ".png" (opaque 24-bit RGB PNG) or ".bmp" (24-bit bottom-up BMP, BI_RGB); missing
   directories are created; an existing file is replaced (written to "<path>.tmp" first, then renamed).
   Pixels: the 8-bit BGRA bytes of the back buffer as the game wrote them (no sRGB conversion), alpha dropped, rows top-down
   in the image: what IDirect3DDevice9::GetRenderTargetData gives under D3D9.
   Returns a ticket (> 0), or 0 when refused: not Vulkan mode, null/relative path, wrong extension, the path resolves into
   the game's data directory (junctions into the game install, also when reached through a link), unknown flag bits, or
   64 requests already pending. Encoding and writing run on a background thread; the present itself stalls only for the
   GPU read-back. */
uint32_t rvk_screenshot(const wchar_t* path, uint32_t wait_frames, uint32_t flags);

/* RVK_SHOT_PENDING / RVK_SHOT_DONE (file written) / RVK_SHOT_FAILED (also for unknown or expired tickets; the last 1024
   finished tickets are remembered). msg (optional, UTF-8, always NUL-terminated when msg_size > 0) receives
   "frame <n>, waited <k> presents, <w>x<h>" when done, the reason when failed, "waiting <k> presents" / "writing" while
   pending. */
int32_t rvk_screenshot_status(uint32_t ticket, char* msg, uint32_t msg_size);

/* Presents so far (the frame counter of the screenshot waits); 0 outside Vulkan mode. */
uint64_t rvk_frame_index(void);

/* Fills `out` (see struct rvk_stats); returns the number of bytes written, 0 when out is null or size < 8. */
int32_t rvk_stats(struct rvk_stats* out, uint32_t size);

/* ---- player settings, A/B, overlay (docs/research/overlay.md 4) ---------------------------------------------------------
   The renderer's player settings (bin\rvk\settings.toml, the in-game overlay) by their stable keys, e.g. "vulkan.vsync",
   "vulkan.max_fps". Available in every mode as soon as the DLL is loaded (they only take effect in Vulkan mode). Values
   travel as text: booleans "true"/"false" (also "on"/"off", "1"/"0"), numbers ("60", "0.5"), enum option names ("off").
   A change applies at the next presented frame (render thread) and is saved to settings.toml about a second later, like a
   change in the overlay. A setting that config.toml sets explicitly is locked for the session (RVK_SETTING_LOCKED). */

/* results (negative = error) */
#define RVK_SETTING_OK 0
#define RVK_SETTING_UNKNOWN_KEY (-1)
#define RVK_SETTING_BAD_VALUE (-2)  /* not parseable for the setting's type / not an option (numbers are clamped, not refused) */
#define RVK_SETTING_LOCKED (-3)     /* config.toml overrides it this session */
#define RVK_SETTING_BAD_ARGUMENT (-4) /* null pointer, unknown flag bits, index out of range, size too small */

/* rvk_setting_info::type */
#define RVK_SETTING_BOOL 0
#define RVK_SETTING_INT 1
#define RVK_SETTING_FLOAT 2
#define RVK_SETTING_ENUM 3

/* rvk_setting_info::flags */
#define RVK_SETTING_RESTART 0x1u          /* takes effect at the next start */
#define RVK_SETTING_NO_AB 0x2u            /* not part of the A/B comparison */
#define RVK_SETTING_HIDDEN 0x4u           /* not shown in the overlay */
#define RVK_SETTING_IS_LOCKED 0x8u        /* config.toml overrides it this session */
#define RVK_SETTING_RESTART_PENDING 0x10u /* changed since the start; needs a restart to take effect */

/* Versioned by size like rvk_stats. Strings are UTF-8, NUL-terminated and owned by the DLL (valid until it unloads). */
struct rvk_setting_info {
    uint32_t size;          /* bytes written by rvk_setting_info */
    uint32_t type;          /* RVK_SETTING_BOOL .. RVK_SETTING_ENUM */
    uint32_t flags;         /* RVK_SETTING_RESTART | ... */
    uint32_t option_count;  /* enum options */
    const char* key;        /* "vulkan.max_fps" */
    const char* label;      /* overlay label */
    const char* category;   /* overlay group */
    const char* description;
    const char* options;    /* enum: the option names, each followed by a newline ("engine", "on", "off" -> 3 lines); "" otherwise */
    double default_value;   /* bool 0/1, enum option index */
    double min;             /* int / float range */
    double max;
};                          /* version 1: 80 bytes */

/* Number of settings (indices 0 .. count-1 for rvk_setting_info; registration order, stable within a build). */
uint32_t rvk_setting_count(void);

/* Fills `out` for setting `index`; returns the bytes written or RVK_SETTING_BAD_ARGUMENT. */
int32_t rvk_setting_info(uint32_t index, struct rvk_setting_info* out, uint32_t size);

/* The current (effective) value of `key` as text into `value` (always NUL-terminated when value_size > 0, truncated if
   needed). Returns the full length of the text (excluding the NUL; > value_size - 1 means truncated) or an error. */
int32_t rvk_setting_get(const char* key, char* value, uint32_t value_size);

/* Sets `key` from text. flags: none defined, must be 0. Returns RVK_SETTING_OK or an error. */
int32_t rvk_setting_set(const char* key, const char* value, uint32_t flags);

/* rvk_ab() ops. Side A is the session's starting values, side B a stored second set (initially the defaults: the plain
   renderer). Flipping swaps them for every A/B setting; the overlay shows which side is active. */
#define RVK_AB_QUERY 0
#define RVK_AB_FLIP 1
#define RVK_AB_SIDE_A 2
#define RVK_AB_SIDE_B 3
#define RVK_AB_COPY 4   /* copy the active side's values to the other side */
#define RVK_AB_RESET 5  /* reset the other side to the defaults */

/* Performs `op` and returns the active side afterwards: 0 = A, 1 = B; RVK_SETTING_BAD_ARGUMENT for an unknown op. */
int32_t rvk_ab(int32_t op);

/* rvk_overlay() ops */
#define RVK_OVERLAY_QUERY 0
#define RVK_OVERLAY_OPEN 1
#define RVK_OVERLAY_CLOSE 2
#define RVK_OVERLAY_TOGGLE 3

/* Opens / closes the in-game overlay; returns 1 = open, 0 = closed afterwards, -1 = no overlay (not Vulkan mode, disabled,
   or the device does not exist yet), RVK_SETTING_BAD_ARGUMENT for an unknown op. */
int32_t rvk_overlay(int32_t op);

#ifdef __cplusplus
}
#endif

#endif /* RVK_API_H */
