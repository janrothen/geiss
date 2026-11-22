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

#include <array>
#include <cstdint>

namespace geiss {

// One color on the simulation's 0...255 scale per channel. The presenter doubles it, as the
// original's truecolor blit did, so 128 already shows as full brightness.
struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;

    friend bool operator==(const Rgb&, const Rgb&) = default;
};

// The original's slow color drift: gF[0...5], six rates of 0.02...0.03 per reference frame,
// and chaser_offset, a phase of 0...39999 reference frames. FX_Init picked them once per run.
struct ColorDrift {
    std::array<float, 6> rates{0.025f, 0.025f, 0.025f, 0.025f, 0.025f, 0.025f};
    double offset = 0;
};

ColorDrift randomColorDrift(Random& random);

// The waveform's color for brightness `base` (0...155, RenderWave's scale) at `frames`
// reference frames, from the colour spectrum: bass red, middle green, treble blue, scaled to
// `base`, with 3% of the drift mixed in and the whole raised by 1.35.
// Without spectrum power (silence) it is `driftColor` instead.
Rgb waveColor(float base, double frames, const ColorDrift& drift, const std::array<float, kFourierDetail>& power);

// The waveform's color without the spectrum: `base` tinted by the drift, each channel
// 0.36...2.49 times it before the 0...255 clamp.
Rgb driftColor(float base, double frames, const ColorDrift& drift);

// Per-channel multipliers for a tinted effect.
struct Tint {
    float r = 1;
    float g = 1;
    float b = 1;
};

// RenderDots' tint for the nuclide's rings with sound: each channel 0.16...1.
Tint nuclideTint(double frames, const ColorDrift& drift);

// Nuclide()'s tint for the random rings in silence: each channel 0...1. The original wrote
// its red to the blue channel and its blue to the red one, and so does this.
Tint silentNuclideTint(double frames, const ColorDrift& drift);

}  // namespace geiss
