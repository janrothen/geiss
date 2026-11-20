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

#include "geiss/audio.hpp"
#include "geiss/map.hpp"

namespace geiss {

// The original's slide shift (main.cpp: slider1, g_ShiftMaxVol, g_FramesSinceShift).
// While a look allows it, each loud peak in beat mode moves the warp's source by a few
// pixels sideways, alternating direction, and up, down, or not at all. The picture then
// drifts that way every frame until the next peak.
class SlideShift {
public:
    // At the original's default g_SlideShiftFreq, 33 looks in 100 allow the shift.
    static constexpr int kChancePercent = 33;

    explicit SlideShift(std::uint32_t seed);

    // A new look: `allowed` comes from `rollAllowed`. A look without the shift stops the
    // drift. One with it keeps the previous drift until a peak replaces it, as in the original.
    void startLook(bool allowed);
    static bool rollAllowed(Random& random);

    // `dt` is seconds of picture motion. `width` sizes the shift.
    void update(double dt, const AudioFrame& audio, int width);

    bool allowed() const { return allowed_; }
    // Pixels per reference frame (1/30 s) at the current width. Positive x reads from the
    // right, so the picture slides left. Positive y reads from below, so it slides up.
    float x() const { return x_; }
    float y() const { return y_; }
    // How many shifts have happened.
    int count() const { return count_; }

private:
    Random random_;
    bool allowed_ = false;
    float x_ = 0;
    float y_ = 0;
    // The peak to beat. A louder bass sets it 5% above itself; otherwise it relaxes
    // toward 1.43 times the bass average.
    float peak_ = 0;
    // Reference frames since the last shift, counted only on frames without a new peak.
    double sinceShift_ = 0;
    int count_ = 0;
};

}  // namespace geiss
