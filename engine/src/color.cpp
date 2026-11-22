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
#include "geiss/color.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace geiss {
namespace {

int channel(double value) { return std::clamp(static_cast<int>(value), 0, 255); }

// The slow wobble f that every drifting color adds to its phases.
double waveWobble(double t) { return 7.0 * std::sin(t * 0.006 + 59) + 5.0 * std::cos(t * 0.0077 + 17); }
double nuclideWobble(double t) { return 7.0 * std::sin(t * 0.007 + 29) + 5.0 * std::cos(t * 0.0057 + 27); }

struct Channels {
    double r;
    double g;
    double b;
};

// base * 1.07 * (1 + c1 sin(...)) * (1 + c2 cos(...)) per channel, as RenderWave tinted it.
Channels drifted(double base, double t, const ColorDrift& drift, double c1, double c2) {
    const double f = waveWobble(t);
    const auto& gF = drift.rates;
    return {
        base * 1.07 * (1 + c1 * std::sin(t * gF[0] + 10 - f)) * (1 + c2 * std::cos(t * gF[1] + 37 + f)),
        base * 1.07 * (1 + c1 * std::sin(t * gF[2] + 32 + f)) * (1 + c2 * std::cos(t * gF[3] + 16 - f)),
        base * 1.07 * (1 + c1 * std::sin(t * gF[4] + 87 - f)) * (1 + c2 * std::cos(t * gF[5] + 25 + f)),
    };
}

double bandSum(const std::array<float, kFourierDetail>& power, std::size_t first, std::size_t end) {
    return std::accumulate(power.begin() + static_cast<std::ptrdiff_t>(first),
                           power.begin() + static_cast<std::ptrdiff_t>(end), 0.0);
}

}  // namespace

ColorDrift randomColorDrift(Random& random) {
    ColorDrift drift;
    for (float& rate : drift.rates) {
        rate = random.unit() * 0.01f + 0.02f;
    }
    drift.offset = random.below(40000);
    return drift;
}

Rgb driftColor(float base, double frames, const ColorDrift& drift) {
    // c1 0.55 and c2 0.50, the richer colors of version 4.26.
    const Channels c = drifted(base, frames + drift.offset, drift, 0.55, 0.50);
    return {channel(c.r), channel(c.g), channel(c.b)};
}

// Intentional difference: the original's sync path counted frames, not 30 fps time.
Rgb waveColor(float base, double frames, const ColorDrift& drift, const std::array<float, kFourierDetail>& power) {
    // Red is bins 0...7, green 7...14, and blue 13...23: the original's int loop counters
    // turned 24 * 0.31, 0.30, 0.59, and 0.56 into these, so bins 7, 13, and 14 count twice.
    double r = bandSum(power, 0, 8) * 0.93;
    const double g = bandSum(power, 7, 15) * 1.18;
    const double b = bandSum(power, 13, kFourierDetail) * 2.40;
    r -= b * 0.4;
    const double length = std::sqrt(r * r + g * g + b * b);
    // Intentional difference: the original divided by zero in silence.
    if (!(length > 1e-9)) {
        return driftColor(base, frames, drift);
    }
    const double scale = static_cast<double>(base) / length;
    const Channels tint = drifted(base, frames + drift.offset, drift, 0.3, 0.2);
    return {
        channel((r * scale * 0.97 + tint.r * 0.03) * 1.35),
        channel((g * scale * 0.97 + tint.g * 0.03) * 1.35),
        channel((b * scale * 0.97 + tint.b * 0.03) * 1.35),
    };
}

namespace {

Tint ringTint(double frames, const ColorDrift& drift, double middle, double swing) {
    const double t = frames + drift.offset;
    const double f = nuclideWobble(t);
    const auto& gF = drift.rates;
    return {
        static_cast<float>(middle + swing * std::sin(t * gF[0] + 20 - f) + swing * std::cos(t * gF[3] + 17 + f)),
        static_cast<float>(middle + swing * std::sin(t * gF[1] + 42 + f) + swing * std::cos(t * gF[4] + 26 - f)),
        static_cast<float>(middle + swing * std::sin(t * gF[2] + 57 - f) + swing * std::cos(t * gF[5] + 35 + f)),
    };
}

}  // namespace

Tint nuclideTint(double frames, const ColorDrift& drift) { return ringTint(frames, drift, 0.58, 0.21); }

Tint silentNuclideTint(double frames, const ColorDrift& drift) {
    const Tint tint = ringTint(frames, drift, 0.5, 0.25);
    return {tint.b, tint.g, tint.r};
}

}  // namespace geiss
