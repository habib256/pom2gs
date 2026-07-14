// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// Audio host. See Audio.h. Source of truth for the miniaudio setup: POM2's
// AudioDevice, trimmed to a single self-mixing source.

#include "Audio.h"
#include "IIgsMemory.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

// This is the one translation unit that instantiates miniaudio. The Web Audio
// backend needs extra Emscripten link flags (audio worklet / pthreads), so on
// WASM we compile a silent stub instead — native desktop is the sound target.
// GCC's -Wstringop-overflow trips a false positive on miniaudio's atomic
// intrinsics (as in POM2) — silence locally.
#ifndef __EMSCRIPTEN__
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
#define MINIAUDIO_IMPLEMENTATION
#include "third_party/miniaudio.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif // __EMSCRIPTEN__

namespace {
// Speaker square-wave amplitude and DOC mix gain. The speaker is the dominant
// voice for 8-bit software; keep it comfortably below full scale so it doesn't
// clip when it stacks with the DOC.
constexpr float kSpkAmp   = 0.22f;
constexpr float kDocGain  = 0.55f / 32768.0f;
// Fast-side clock (2.8 MHz). Only used as a sanity reference; the per-frame
// sample count is derived from sampleRate/60 so it never drifts against the
// device rate.
}

AudioOut::AudioOut()
    : ring_(kRingSize, 0.0f)
{
#ifndef __EMSCRIPTEN__
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format    = ma_format_f32;
    cfg.playback.channels  = 1;
    cfg.sampleRate         = 44100;
    cfg.periodSizeInFrames = 256;
    cfg.periods            = 3;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.dataCallback       = &AudioOut::dataCallback;
    cfg.pUserData          = this;

    ma_device* raw = new ma_device();
    if (ma_device_init(nullptr, &cfg, raw) != MA_SUCCESS) {
        delete raw;
        std::fprintf(stderr, "Audio: ma_device_init failed — sound disabled\n");
        return;
    }
    if (ma_device_start(raw) != MA_SUCCESS) {
        ma_device_uninit(raw);
        delete raw;
        std::fprintf(stderr, "Audio: ma_device_start failed — sound disabled\n");
        return;
    }
    sampleRate_ = raw->sampleRate ? raw->sampleRate : 44100;
    device_ = raw;
    available_ = true;
    std::printf("Audio: miniaudio ready (%u Hz)\n", sampleRate_);
#endif
}

AudioOut::~AudioOut()
{
#ifndef __EMSCRIPTEN__
    if (device_) {
        ma_device_uninit(device_);
        delete device_;
        device_ = nullptr;
    }
#endif
}

// ── SPSC ring ────────────────────────────────────────────────────────────
void AudioOut::push(const float* s, int n)
{
    uint32_t w = write_.load(std::memory_order_relaxed);
    const uint32_t r = read_.load(std::memory_order_acquire);
    const uint32_t used = w - r;
    uint32_t freeSlots = uint32_t(kRingSize) - used;
    if (freeSlots == 0) return;                    // ring full — drop (device stalled)
    int toWrite = std::min<int>(n, int(freeSlots));
    for (int i = 0; i < toWrite; ++i)
        ring_[(w + uint32_t(i)) & (kRingSize - 1)] = s[i];
    write_.store(w + uint32_t(toWrite), std::memory_order_release);
}

void AudioOut::pull(float* out, int n)
{
    uint32_t r = read_.load(std::memory_order_relaxed);
    const uint32_t w = write_.load(std::memory_order_acquire);
    const uint32_t avail = w - r;
    int toRead = std::min<int>(n, int(avail));
    for (int i = 0; i < toRead; ++i)
        out[i] = ring_[(r + uint32_t(i)) & (kRingSize - 1)];
    for (int i = toRead; i < n; ++i) out[i] = 0.0f;   // underflow → silence
    read_.store(r + uint32_t(toRead), std::memory_order_release);
}

#ifndef __EMSCRIPTEN__
void AudioOut::dataCallback(ma_device* dev, void* out, const void* /*in*/, uint32_t frames)
{
    auto* self = static_cast<AudioOut*>(dev->pUserData);
    float* o = static_cast<float*>(out);
    if (!self) { std::fill(o, o + frames, 0.0f); return; }
    self->pull(o, int(frames));
}
#endif

// ── Per-frame mix ────────────────────────────────────────────────────────
void AudioOut::mixFrame(IIgsMemory& mem)
{
    const uint64_t cyc1 = mem.audioCycles();
    std::vector<uint64_t> ev;
    mem.takeSpeakerEvents(ev);

    if (!primed_) { lastCyc_ = cyc1; primed_ = true; }   // first frame sets the origin
    const uint64_t cyc0 = lastCyc_;
    lastCyc_ = cyc1;

    // One frame worth of samples. 44100/60 and 48000/60 are both integral.
    const int n = int(sampleRate_ / 60);
    if (n <= 0) return;
    const uint64_t span = (cyc1 > cyc0) ? (cyc1 - cyc0) : 1;

    if (int(mixBuf_.size()) < n) mixBuf_.resize(n);
    if (int(docBuf_.size()) < n) docBuf_.resize(n);

    // Speaker: walk the toggle stamps, flipping the level as each is crossed.
    size_t ei = 0;
    for (int s = 0; s < n; ++s) {
        const uint64_t sampleCyc = cyc0 + (uint64_t(s) * span) / uint64_t(n);
        while (ei < ev.size() && ev[ei] <= sampleCyc) { spkLevel_ = !spkLevel_; ++ei; }
        mixBuf_[s] = spkLevel_ ? kSpkAmp : -kSpkAmp;
    }
    // Any events past the last sampled cycle still flip the persistent level.
    for (; ei < ev.size(); ++ei) spkLevel_ = !spkLevel_;

    // DOC: render and add. Silent (all oscillators halted) at boot → no-op.
    mem.docChip().render(docBuf_.data(), n);
    for (int s = 0; s < n; ++s) {
        float v = mixBuf_[s] + float(docBuf_[s]) * kDocGain;
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        mixBuf_[s] = v;
    }

    // Master volume / mute.
    const float g = muted_.load(std::memory_order_relaxed) ? 0.0f
                                                           : volume_.load(std::memory_order_relaxed);
    if (g != 1.0f) for (int s = 0; s < n; ++s) mixBuf_[s] *= g;

    push(mixBuf_.data(), n);
}
