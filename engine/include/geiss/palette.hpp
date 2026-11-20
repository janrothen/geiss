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

#include "geiss/map.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace geiss {

// 256 RGB entries. Index 0 is where the warp fades to.
using Palette = std::array<std::uint8_t, std::size_t{256} * 3>;

constexpr int kFirstPaletteCurve = 1;
constexpr int kLastPaletteCurve = 7;
// CrankPal curve 6 is very dark. FX_Random_Palette re-rolls until at most one channel uses it.
constexpr int kDarkPaletteCurve = 6;
// Curve 7 ripples. It is only offered on the "solar" rolls.
constexpr int kSolarPaletteCurve = 7;

// What FX_Random_Palette chose. Lock holds it with the rest of the look.
struct PaletteRecipe {
    // The monotone route takes one of four fixed orderings of three curves.
    bool monotone = false;
    int monotoneOrder = 0;
    // The curve route takes one CrankPal curve per channel, in the original's order:
    // the first drives red, the second blue, and the third green.
    std::array<int, 3> curves{2, 2, 2};
    // The coarse band doubles the indices strictly between these. -1 when it is off.
    int lowBand = -1;
    int highBand = -1;

    bool operator==(const PaletteRecipe&) const = default;
};

// CrankPal: curve `id` at palette index 0...255, before gamma and clamping.
float paletteCurve(int id, int index);

// Clamps curves to 1...7 and the monotone order to 0...3, so every recipe builds a real palette.
PaletteRecipe clampedPaletteRecipe(PaletteRecipe recipe);

// FX_Random_Palette's pick at the original's default settings (coarse and solar frequency 1).
PaletteRecipe randomPaletteRecipe(Random& random);

// The table FX_Random_Palette builds, from the clamped recipe. `soundEmpty` adds the original's +0.3 gamma after a long silence.
Palette buildPalette(const PaletteRecipe& recipe, bool soundEmpty);

// `t` 0 is `from`, 1 is `to`. PutPalette blended linearly.
Palette blendPalettes(const Palette& from, const Palette& to, float t);

// One highlight level per palette index, for HDR highlights. A level is the index, capped by
// the entry's luminance next to the palette's brightest entry, both on a 0...255 scale. So a
// pixel glows only when it is fresh (a high index) and among the palette's brightest colors.
// Bright bands in the middle of a palette are trails and stay at SDR, and dim colors at the
// top of a rippling palette do not glow. A black palette has no highlights.
using HighlightLevels = std::array<std::uint8_t, 256>;
HighlightLevels highlightLevels(const Palette& palette);

}  // namespace geiss
