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

#include "geiss/shift.hpp"

#include <algorithm>
#include <cmath>

namespace geiss {
namespace {

constexpr double kReferenceFps = 30.0;
// g_ShiftMaxVol = current_vol * 1.05 on a new peak.
constexpr float kPeakHeadroom = 1.05f;
// Otherwise it relaxes toward avg_vol_uniform * 1.43 at 0.975 per frame at 30 fps.
constexpr float kPeakFloor = 1.43f;
constexpr double kPeakRetain = 0.975;
// A shift needs more than 2 frames without a peak since the previous one.
constexpr double kMinFramesBetween = 2.0;
// The original's sizes are for a 640-wide frame.
constexpr float kReferenceWidth = 640.f;
constexpr int kReferenceHalfWidth = 320;
// slider1 = ((rand() % (FXW/2)) + 50) / 145: 0, 1, or 2 pixels at 640 wide.
constexpr int kShiftBase = 50;
constexpr int kShiftDivisor = 145;

}  // namespace

SlideShift::SlideShift(std::uint32_t seed) { random_.state = seed == 0 ? 1u : seed; }

bool SlideShift::rollAllowed(Random& random) { return random.below(100) + 1 <= kChancePercent; }

void SlideShift::startLook(bool allowed) {
    allowed_ = allowed;
    if (!allowed_) {
        x_ = 0;
        y_ = 0;
    }
}

void SlideShift::update(double dt, const AudioFrame& audio, int width) {
    if (!allowed_ || !audio.beatMode || dt <= 0) {
        return;
    }
    const double frames = dt * kReferenceFps;
    // Intentional difference: the peaks follow the bass level that drives beat mode,
    // instead of the composite volume.
    if (audio.bass > peak_) {
        peak_ = audio.bass * kPeakHeadroom;
        if (sinceShift_ > kMinFramesBetween) {
            sinceShift_ = 0;
            ++count_;
            const float scale = static_cast<float>(width) / kReferenceWidth;
            // The original's integer division truncates to whole pixels.
            const int wholePixels = (random_.below(kReferenceHalfWidth) + kShiftBase) / kShiftDivisor;
            const auto pixels = static_cast<float>(wholePixels);
            // The original flipped the sign when the whole previous offset was positive,
            // vertical part included. Intentional difference: only the sideways part counts,
            // so the direction always alternates as the original intended.
            const bool wasRight = x_ > 0.f;
            x_ = (wasRight ? -pixels : pixels) * scale;
            y_ = static_cast<float>(random_.below(3) - 1) * scale;
        }
    } else {
        const auto retain = static_cast<float>(std::pow(kPeakRetain, frames));
        peak_ = peak_ * retain + audio.bassAverage * kPeakFloor * (1.f - retain);
        sinceShift_ += frames;
    }
}

}  // namespace geiss
