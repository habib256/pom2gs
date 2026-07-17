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
constexpr float kDocGain  = 0.95f / 32768.0f;   // music near full scale (final mix clamps)
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
    // Pre-fill ~35 ms of silence (the rate-control band's centre): the device
    // starts pulling immediately but the first mixFrame lands a frame later —
    // the empty-ring gap was ~55 underruns in the first second (a crackly boot
    // beep).
    write_.store(1536, std::memory_order_release);   // ring_ is zero-initialised
    std::printf("Audio: miniaudio ready (%u Hz)\n", sampleRate_);
    // POMWAV=<path>: record the mixed output for offline crackle analysis.
    if (const char* wp = std::getenv("POMWAV")) {
        wavFile_ = std::fopen(wp, "wb");
        if (wavFile_) {
            uint8_t hdr[44] = {0};                    // placeholder; fixed in dtor
            std::fwrite(hdr, 1, 44, wavFile_);
            std::printf("Audio: recording to %s\n", wp);
        }
    }
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
    if (wavFile_) {                                   // finalise the POMWAV header
        const uint32_t dlen = wavSamples_ * 2, rlen = 36 + dlen, fmtlen = 16;
        const uint32_t rate = sampleRate_, bps = rate * 2;
        const uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
        std::fseek(wavFile_, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, wavFile_); std::fwrite(&rlen, 4, 1, wavFile_);
        std::fwrite("WAVEfmt ", 1, 8, wavFile_);
        std::fwrite(&fmtlen, 4, 1, wavFile_); std::fwrite(&pcm, 2, 1, wavFile_);
        std::fwrite(&ch, 2, 1, wavFile_); std::fwrite(&rate, 4, 1, wavFile_);
        std::fwrite(&bps, 4, 1, wavFile_); std::fwrite(&align, 2, 1, wavFile_);
        std::fwrite(&bits, 2, 1, wavFile_);
        std::fwrite("data", 1, 4, wavFile_); std::fwrite(&dlen, 4, 1, wavFile_);
        std::fclose(wavFile_); wavFile_ = nullptr;
    }
}

// ── SPSC ring ────────────────────────────────────────────────────────────
void AudioOut::push(const float* s, int n)
{
    uint32_t w = write_.load(std::memory_order_relaxed);
    const uint32_t r = read_.load(std::memory_order_acquire);
    const uint32_t used = w - r;
    uint32_t freeSlots = uint32_t(kRingSize) - used;
    if (freeSlots == 0) { drops_.fetch_add(1, std::memory_order_relaxed); return; }   // device stalled
    int toWrite = std::min<int>(n, int(freeSlots));
    if (toWrite < n) drops_.fetch_add(1, std::memory_order_relaxed);                  // partial drop
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
    if (toRead < n) underruns_.fetch_add(1, std::memory_order_relaxed);   // audible gap
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

    // One frame worth of samples (44100/60 = 735) — with DYNAMIC RATE CONTROL:
    // we push vsync-paced (~60 Hz), the device pulls at its own crystal's 44100.
    // The two clocks drift (fractions of a %), so the ring slowly drains or
    // fills until it underruns (a crackle) or drops a burst — the "random
    // cracks" during long music playback. Nudge this frame's sample count a
    // hair (±8 ≈ ±0.65 %, inaudible) to keep the ring centred in a band, the
    // standard emulator fix (KEGS sound.c does the same).
    int n = int(sampleRate_ / 60);
    if (n <= 0) return;
    {
        const uint32_t fill = write_.load(std::memory_order_relaxed)
                            - read_.load(std::memory_order_acquire);
        if (fill < 1100)      n += 8;              // draining → produce a bit more
        else if (fill > 1900) n -= 8;              // backing up → a bit less
    }
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

    // DOC: drain the samples the MMU produced cycle-accurately this frame
    // (IIgsMemory::tick → Es5503::tickMaster), resampled to our rate. Silent
    // (all oscillators halted) at boot → flat line.
    mem.docChip().drainResampled(docBuf_.data(), n, sampleRate_);
    for (int s = 0; s < n; ++s) {
        float v = mixBuf_[s] + float(docBuf_[s]) * kDocGain;
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        mixBuf_[s] = v;
    }

    // AC-couple the mix (one-pole DC blocker, fc ≈ 20 Hz) — a real speaker can't
    // hold DC. The 1-bit speaker level parked the output at a ±kSpkAmp offset,
    // so any device-level gap (an underrun's zero-padding) was a full-amplitude
    // step = a loud click; on a ~0-centred signal the same gap is nearly
    // inaudible. Also gives the square wave the exponential sag of the real
    // cone. Music (no DC) passes through untouched.
    for (int s = 0; s < n; ++s) {
        const float x = mixBuf_[s];
        const float y = x - dcX_ + 0.9971f * dcY_;
        dcX_ = x; dcY_ = y;
        mixBuf_[s] = y;
    }

    // Master volume / mute.
    const float g = muted_.load(std::memory_order_relaxed) ? 0.0f
                                                           : volume_.load(std::memory_order_relaxed);
    if (g != 1.0f) for (int s = 0; s < n; ++s) mixBuf_[s] *= g;

    // Final saturating clamp: the DC blocker (and volume) can transiently push a
    // sample past ±1.0 (~1.44× on a large step); the f32 backend hard-clips and
    // the int16 WAV cast wraps, both audible as clicks. Saturate softly instead.
    for (int s = 0; s < n; ++s) {
        float v = mixBuf_[s];
        mixBuf_[s] = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
    }

    // POMWAV=<path>: append the mixed output to a WAV for offline analysis
    // (crackle hunting — the header is finalised in the destructor).
    if (wavFile_) {
        static std::vector<int16_t> w16; if (int(w16.size()) < n) w16.resize(n);
        for (int s = 0; s < n; ++s) w16[s] = int16_t(mixBuf_[s] * 32767.0f);
        std::fwrite(w16.data(), 2, size_t(n), wavFile_);
        wavSamples_ += uint32_t(n);
    }

    push(mixBuf_.data(), n);
}
