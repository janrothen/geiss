// This file is part of the Geiss macOS port.
//                           ___ ___ ___ ___ ___
//                          / __| __|_ _/ __/ __|
//                         | (_ | _| | |\__ \__ \
//                          \___|___|___|___/___/
// 3-Clause BSD License
//
// Copyright (c) 1998-2022 Ryan Geiss (@geissomatik)
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "geiss/audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace geiss {
namespace {

constexpr float kSilenceLevel = 0.01f;
constexpr double kSilenceHoldSeconds = 0.5;
// 16-bit peak-to-peak / 256 in the original: a float peak-to-peak times 32768 / 256.
constexpr float kVolumeScale = 128.f;
// avg_vol = avg_vol*0.85 + vol*0.15 per frame at 30 fps.
constexpr double kAverageVolumeRetain = 0.85;
constexpr double kVolumeWindowSeconds = 1.0 / 30.0;
constexpr double kOriginalSampleRate = 44100.0;

float downmix(const float* frame, int channels) {
    if (channels <= 1) {
        return frame[0];
    }
    return 0.5f * (frame[0] + frame[1]);
}

std::vector<float> channel(const PcmBlock& block, int which, float gain) {
    std::vector<float> out(static_cast<std::size_t>(block.frameCount));
    for (int i = 0; i < block.frameCount; ++i) {
        const float* sample = block.samples + static_cast<std::size_t>(i * block.channels);
        float value = sample[0];
        if (which < 0) {
            value = downmix(sample, block.channels);
        } else if (which == 1 && block.channels > 1) {
            value = sample[1];
        }
        out[static_cast<std::size_t>(i)] = value * gain;
    }
    return out;
}

// Resamples source[first, first + count) onto dest's columns.
void resample(const std::vector<float>& source, std::size_t first, std::size_t count, std::vector<float>& dest) {
    const int columns = static_cast<int>(dest.size());
    if (count == 0 || first + count > source.size() || columns < 1) {
        return;
    }
    if (columns == 1 || count == 1) {
        std::fill(dest.begin(), dest.end(), source[first + count / 2]);
        return;
    }
    const float span = static_cast<float>(count - 1);
    const float denom = static_cast<float>(columns - 1);
    for (int i = 0; i < columns; ++i) {
        const float pos = span * (static_cast<float>(i) / denom);
        const auto i0 = static_cast<std::size_t>(pos);
        const std::size_t i1 = std::min(i0 + 1, count - 1);
        const float fraction = pos - static_cast<float>(i0);
        const float a = source[first + i0];
        const float b = source[first + i1];
        dest[static_cast<std::size_t>(i)] = a * (1.f - fraction) + b * fraction;
    }
}

// The original read one stereo frame for every two columns at 44.1 kHz, so the line spans
// columns / 88200 seconds of sound however long the captured block is.
std::size_t waveWindowFrames(int columns, double sampleRate, std::size_t available) {
    const double frames = sampleRate > 0
        ? std::round(static_cast<double>(columns) * sampleRate / (2.0 * kOriginalSampleRate))
        : static_cast<double>(available);
    return std::clamp(static_cast<std::size_t>(std::max(frames, 2.0)), std::size_t{1}, available);
}

// Where the drawn window starts. The newest rising zero crossing that still leaves a whole
// window after it, searched back up to one window from the newest start, so the line holds
// in place. Without one, the window is the newest audio.
// Intentional difference from GetWaveData: a rising edge, no height or slope match across frames.
std::size_t waveWindowStart(const std::vector<float>& mono, std::size_t window) {
    const std::size_t newest = mono.size() - window;
    const std::size_t oldest = newest > window ? newest - window : 0;
    for (std::size_t start = newest; start > oldest; --start) {
        if (mono[start - 1] <= 0.f && mono[start] > 0.f) {
            return start;
        }
    }
    return newest;
}

// Peak-to-peak of the mono samples over the newest 1/30 s, on the original's volume scale.
// Intentional difference: the original measured the left channel of its whole capture buffer.
float volumeOf(const std::vector<float>& mono, double sampleRate) {
    if (mono.empty()) {
        return 0.f;
    }
    const auto size = static_cast<std::ptrdiff_t>(mono.size());
    const auto window = sampleRate > 0
        ? std::clamp(static_cast<std::ptrdiff_t>(sampleRate * kVolumeWindowSeconds), std::ptrdiff_t{1}, size)
        : size;
    const auto [low, high] = std::minmax_element(mono.end() - window, mono.end());
    return (*high - *low) * kVolumeScale;
}

bool hasAudioBlock(const PcmBlock& block) {
    return block.samples != nullptr && block.frameCount > 0 && block.channels > 0;
}

}  // namespace

AudioFrame Analyzer::analyze(const PcmBlock& block, int columns, float gain) {
    AudioFrame frame;
    frame.time = block.time;
    if (gain < 0.f) {
        gain = 0.f;
    }
    double elapsed = 0;
    if (hasLastTime_ && block.time > lastTime_) {
        // A long gap counts as the full hold, not more.
        elapsed = std::min(block.time - lastTime_, kSilenceHoldSeconds);
    } else if (block.sampleRate > 0 && block.frameCount > 0) {
        elapsed = static_cast<double>(block.frameCount) / block.sampleRate;
    }
    lastTime_ = block.time;
    hasLastTime_ = true;

    // Time between blocks, not block length, drives the beat history too.
    const float bass = hasAudioBlock(block)
        ? bassLevel(block.samples, block.frameCount, block.channels, block.sampleRate, gain)
        : 0.f;
    const BeatState beat = beat_.update(bass, elapsed);
    frame.bass = beat.bass;
    frame.beatMode = beat.beatMode;
    frame.beatBrightness = beat.brightness;
    frame.bigBeatRatio = beat.bigBeatRatio;
    if (columns < 1) {
        return frame;
    }

    std::vector<float> mono;
    std::vector<float> left;
    std::vector<float> right;
    const bool hasAudio = hasAudioBlock(block);
    if (hasAudio) {
        mono = channel(block, -1, gain);
        if (block.channels == 1) {
            left = mono;
            right = mono;
        } else {
            left = channel(block, 0, gain);
            right = channel(block, 1, gain);
        }
    }

    double sum = 0;
    float peak = 0;
    for (float sample : mono) {
        sum += static_cast<double>(sample) * static_cast<double>(sample);
        peak = std::max(peak, std::fabs(sample));
    }
    frame.level = mono.empty() ? 0.f : static_cast<float>(std::sqrt(sum / static_cast<double>(mono.size())));
    frame.peak = peak;
    frame.volume = volumeOf(mono, block.sampleRate);
    const auto retain = static_cast<float>(std::pow(kAverageVolumeRetain, elapsed * 30.0));
    averageVolume_ = averageVolume_ * retain + frame.volume * (1.f - retain);
    frame.averageVolume = averageVolume_;

    frame.samples.assign(static_cast<std::size_t>(columns), 0.f);
    frame.left.assign(static_cast<std::size_t>(columns), 0.f);
    frame.right.assign(static_cast<std::size_t>(columns), 0.f);
    if (!mono.empty()) {
        const std::size_t window = waveWindowFrames(columns, block.sampleRate, mono.size());
        const std::size_t first = waveWindowStart(mono, window);
        resample(mono, first, window, frame.samples);
        resample(left, first, window, frame.left);
        resample(right, first, window, frame.right);
    }

    if (frame.level < kSilenceLevel) {
        silenceHeld_ += elapsed;
    } else {
        silenceHeld_ = 0;
    }
    frame.silent = silenceHeld_ >= kSilenceHoldSeconds;
    return frame;
}

}  // namespace geiss
