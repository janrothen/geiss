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

#pragma once

#include "geiss/beat.hpp"

#include <array>
#include <vector>

namespace geiss {

// Bass, middle, and treble, for the spectral dots.
constexpr int kSpectrumBands = 3;

// FOURIER_DETAIL: the bands of the original's colour spectrum.
constexpr int kFourierDetail = 24;

// Center of colour band `band`, 20 * 2^(10 * band / 24) Hz: about 27 Hz for band 1 up to
// 15.3 kHz for band 23. Band 0 is never measured, as in the original.
double fourierBandHz(int band);

// One captured block. `samples` is interleaved and owned by the caller.
struct PcmBlock {
    const float* samples = nullptr;
    int frameCount = 0;
    int channels = 0;
    double sampleRate = 0;
    double time = 0;
};

// What the renderer is allowed to see.
// `samples`, `left`, and `right` each hold one value per column, about −1…1.
// A one-channel block copies that channel into both sides.
struct AudioFrame {
    double time = 0;
    std::vector<float> samples;
    std::vector<float> left;
    std::vector<float> right;
    float level = 0;
    float peak = 0;
    // The original's volume scale (16-bit peak-to-peak / 256, so 0...256): peak-to-peak
    // of the mono samples over the newest 1/30 s, after gain. `averageVolume` smooths it
    // like avg_vol (0.85 per frame at 30 fps). RenderWave's brightness uses both.
    float volume = 0;
    float averageVolume = 0;
    bool silent = false;
    // Bass level and the beat state derived from it. See BeatDetector.
    float bass = 0;
    bool beatMode = false;
    float beatBrightness = 1;
    float bigBeatRatio = 0;
    float bassAverage = 0;
    // Bass (below 250 Hz), middle (250 Hz...2 kHz), and treble (above 2 kHz) on the scale of
    // the original's spectral dots, 0...255: the mean absolute level over the newest 1/30 s,
    // after gain, weighted up toward the treble.
    std::array<float, kSpectrumBands> spectrum{};
    // g_power_smoothed: the strength of each colour band, smoothed over about half a second.
    // Band 0 stays 0. Only the bands' proportions matter; the wave colour normalizes them.
    std::array<float, kFourierDetail> power{};
};

// The original's colour spectrum for one block, unsmoothed: the magnitude of a direct
// Fourier sum at each band's center over `count` samples (256 in the original), after its
// 0.8/0.2 smoothing of each sample with the next one and with the mean taken out.
std::array<float, kFourierDetail> fourierPower(const float* samples, int count, double sampleRate);

// Turns PCM into an AudioFrame, including the beat state from BeatDetector.
// The columns span columns / 88200 s of the newest audio, as the original's one stereo
// frame per two columns at 44.1 kHz did, whatever the block length. Following the level
// trigger in GetWaveData, that window starts at a rising zero crossing, and the mono, left,
// and right columns share it.
// The silence hold counts the time between blocks, from `PcmBlock::time`, so overlapping
// blocks and the caller's frame rate do not change it. When the time does not advance,
// the block's own duration counts instead.
class Analyzer {
public:
    AudioFrame analyze(const PcmBlock& block, int columns, float gain);

private:
    BeatDetector beat_;
    float averageVolume_ = 0;
    std::array<float, kFourierDetail> power_{};
    double silenceHeld_ = 0;
    double lastTime_ = 0;
    bool hasLastTime_ = false;
};

}  // namespace geiss
