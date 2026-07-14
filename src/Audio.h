// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// ── Audio output (miniaudio) ─────────────────────────────────────────────
// One mono float32 playback device. The emulator thread calls mixFrame()
// once per emulated video frame; it reconstructs the 1-bit speaker square
// wave from the MMU's $C030 toggle cycle-stamps and mixes in the Ensoniq
// 5503 DOC's oscillator output, then pushes ~sampleRate/60 samples into a
// lock-free ring. miniaudio's callback thread drains the ring (silence on
// underflow). No shared mutable state beyond the SPSC ring indices.
//
// The speaker is cycle-exact: within a frame the [cyc0,cyc1) span is mapped
// linearly onto the frame's sample slice, so 8-bit software (Total Replay,
// classic Apple II games) sounds at the right pitch. The DOC is rendered at
// the output rate (pitch is approximate — see TODO P1 "DOC accuracy").

#ifndef POMIIGS_AUDIO_H
#define POMIIGS_AUDIO_H

#include <atomic>
#include <cstdint>
#include <vector>

class IIgsMemory;
struct ma_device;

class AudioOut
{
public:
    AudioOut();
    ~AudioOut();

    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    bool     available() const { return available_; }
    uint32_t sampleRate() const { return sampleRate_; }

    // Master volume [0,1] and mute (bound to the ImGui panel).
    void  setVolume(float v) { volume_.store(v < 0 ? 0 : (v > 1 ? 1 : v), std::memory_order_relaxed); }
    float volume() const { return volume_.load(std::memory_order_relaxed); }
    void  setMuted(bool m) { muted_.store(m, std::memory_order_relaxed); }
    bool  muted() const { return muted_.load(std::memory_order_relaxed); }

    // Call once per emulated video frame, after the frame's CPU cycles have
    // run. `mem` supplies the speaker toggle events (drained) + the DOC.
    void mixFrame(IIgsMemory& mem);

private:
    static constexpr int kRingSize = 1 << 15;   // 32768 float samples (~0.74 s @ 44.1k)

    void push(const float* s, int n);
    void pull(float* out, int n);
    static void dataCallback(ma_device* dev, void* out, const void* in, uint32_t frames);

    ma_device* device_ = nullptr;
    bool       available_ = false;
    uint32_t   sampleRate_ = 44100;

    // SPSC ring: producer = emulator thread (write_), consumer = audio thread (read_).
    std::vector<float>    ring_;
    std::atomic<uint32_t> write_{0};
    std::atomic<uint32_t> read_{0};

    std::atomic<float> volume_{0.7f};
    std::atomic<bool>  muted_{false};

    // Speaker reconstruction state (persists across frames).
    bool     spkLevel_ = false;
    uint64_t lastCyc_  = 0;
    bool     primed_   = false;

    std::vector<int16_t> docBuf_;   // scratch for the DOC render
    std::vector<float>   mixBuf_;   // scratch for the per-frame mix
};

#endif // POMIIGS_AUDIO_H
