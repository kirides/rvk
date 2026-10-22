#pragma once

// [debug] present_stalls (docs/research/fps-limit.md): where does a thread spend its time inside one watched call (the
// present thread's vkQueuePresentKHR in Vulkan mode, the engine's SwapScreen = IDirect3DDevice9::Present otherwise)?
//
// The watched thread brackets the call with enter() / leave(). A helper thread wakes every ~0.5 ms; while a call is in
// progress for longer than the threshold it suspends the watched thread, walks its stack with RtlVirtualUnwind into a fixed
// buffer (nothing allocates while the thread is suspended: it may hold the heap lock), resumes it and counts the stack.
// Every 5 s the helper thread logs the call rate / durations and the most frequent stacks by module + nearest export
// ("ntdll!NtWaitForSingleObject+0x14 < amdvlk64+0x1a2b").
// A debug tool: suspending a thread that holds the loader lock while we look up unwind data could stall the sampler; it
// is never on in normal play.

#include <cstdint>
#include <string>

namespace rvk::diag {

class StallSampler;

// The process-wide present sampler; null unless [debug] present_stalls is on.
StallSampler* presentSampler();
// Creates it (bootstrap, once). thresholdMs: samples are taken only once a call has lasted that long.
void enablePresentSampler(const char* what, double thresholdMs);

class StallSampler {
public:
    StallSampler(std::string what, double thresholdMs);
    ~StallSampler();
    StallSampler(const StallSampler&) = delete;
    StallSampler& operator=(const StallSampler&) = delete;

    // the watched thread (any thread may call; the sampler follows the thread of the latest enter())
    void enter();
    void leave();

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace rvk::diag
