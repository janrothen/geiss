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

#include "geiss/palette.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace geiss {
namespace {

// The original's default gamma setting of 10 gives 1 + 10 * 0.01.
constexpr float kGammaFactor = 1.1f;
// FX_Random_Palette brightened the palette by 0.3 more when it was built in silence.
constexpr float kSilentGammaBoost = 0.3f;
// At the default coarse_pal_freq of 1, rand()%10 < 1.
constexpr int kCoarseChanceOutOf10 = 1;
// At the default solar_pal_freq of 1, rand()%5 < 1.
constexpr int kSolarChanceOutOf5 = 1;
// b = rand()%6, and b == 0 takes the monotone route.
constexpr int kRoutes = 6;
constexpr int kMonotoneOrders = 4;
// The monotone tables ramp over 0...127 and hold the value at 127 above that.
constexpr int kMonotoneRamp = 128;

std::uint8_t toChannel(float value) {
    // The original assigned min(255, x) to an unsigned char, which truncates.
    return static_cast<std::uint8_t>(std::clamp(value, 0.f, 255.f));
}

// The three curves the monotone route orders: a*a/64, a*2, and sqrt(a)*22.6.
float squareRamp(float a) { return a * a / 64.f; }
float linearRamp(float a) { return a * 2.f; }
float rootRamp(float a) { return std::sqrt(a) * 22.6f; }

}  // namespace

float paletteCurve(int id, int index) {
    const auto x = static_cast<float>(index);
    switch (id) {
        case 1: return std::sqrt(x) * 22.6f;
        case 2: return x * 2.f;
        case 3: return x * x / 64.f;
        case 4: return 255.f * std::sin(x / 256.f * 0.5f * std::numbers::pi_v<float>);
        case 5: return x * 3.5f;
        case 6: return std::pow(1.5f, x / 20.f) - 1.f;
        case 7: return x * 1.5f + 128.f * 0.25f + 128.f * 0.25f * std::sin(x * 0.3f);
        default: return 255.f;
    }
}

PaletteRecipe clampedPaletteRecipe(PaletteRecipe recipe) {
    recipe.monotoneOrder = std::clamp(recipe.monotoneOrder, 0, kMonotoneOrders - 1);
    for (int& curve : recipe.curves) {
        curve = std::clamp(curve, kFirstPaletteCurve, kLastPaletteCurve);
    }
    return recipe;
}

PaletteRecipe randomPaletteRecipe(Random& random) {
    PaletteRecipe recipe;
    if (random.below(10) < kCoarseChanceOutOf10) {
        recipe.lowBand = 7 + random.below(6);
        recipe.highBand = 17 + random.below(6);
    }
    if (random.below(kRoutes) == 0) {
        recipe.monotone = true;
        recipe.monotoneOrder = random.below(kMonotoneOrders);
        return recipe;
    }
    int dark = 0;
    do {
        const int count = random.below(5) < kSolarChanceOutOf5 ? kLastPaletteCurve : kLastPaletteCurve - 1;
        dark = 0;
        for (int& curve : recipe.curves) {
            curve = kFirstPaletteCurve + random.below(count);
            dark += curve == kDarkPaletteCurve ? 1 : 0;
        }
    } while (dark > 1);
    return recipe;
}

Palette buildPalette(const PaletteRecipe& unclamped, bool soundEmpty) {
    const PaletteRecipe recipe = clampedPaletteRecipe(unclamped);
    Palette palette{};
    for (int n = 0; n < 256; ++n) {
        float red = 0;
        float green = 0;
        float blue = 0;
        if (recipe.monotone) {
            // The monotone route has no gamma and no coarse band, as in the original.
            const auto a = static_cast<float>(std::min(n, kMonotoneRamp - 1));
            switch (recipe.monotoneOrder) {
                case 1: red = squareRamp(a); blue = rootRamp(a); green = linearRamp(a); break;
                case 2: red = rootRamp(a); blue = linearRamp(a); green = squareRamp(a); break;
                case 3: red = linearRamp(a); blue = squareRamp(a); green = rootRamp(a); break;
                default: red = squareRamp(a); blue = linearRamp(a); green = rootRamp(a); break;
            }
        } else {
            const float gamma = kGammaFactor + (soundEmpty ? kSilentGammaBoost : 0.f);
            const float band = n > recipe.lowBand && n < recipe.highBand ? 2.f : 1.f;
            red = paletteCurve(recipe.curves[0], n) * gamma * band;
            blue = paletteCurve(recipe.curves[1], n) * gamma * band;
            green = paletteCurve(recipe.curves[2], n) * gamma * band;
        }
        const auto i = static_cast<std::size_t>(n) * 3;
        palette[i] = toChannel(red);
        palette[i + 1] = toChannel(green);
        palette[i + 2] = toChannel(blue);
    }
    return palette;
}

Palette blendPalettes(const Palette& from, const Palette& to, float t) {
    t = std::clamp(t, 0.f, 1.f);
    Palette out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float a = static_cast<float>(from[i]);
        const float b = static_cast<float>(to[i]);
        out[i] = static_cast<std::uint8_t>(a + (b - a) * t + 0.5f);
    }
    return out;
}

namespace {

// Linear-light luminance of an sRGB-encoded entry, with Rec. 709 weights.
float luminance(const Palette& palette, std::size_t index) {
    const auto linear = [](std::uint8_t channel) {
        const float c = static_cast<float>(channel) / 255.f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * linear(palette[index * 3]) + 0.7152f * linear(palette[index * 3 + 1]) +
           0.0722f * linear(palette[index * 3 + 2]);
}

}  // namespace

HighlightLevels highlightLevels(const Palette& palette) {
    std::array<float, 256> luma{};
    float brightest = 0;
    for (std::size_t n = 0; n < luma.size(); ++n) {
        luma[n] = luminance(palette, n);
        brightest = std::max(brightest, luma[n]);
    }
    HighlightLevels levels{};
    if (brightest <= 0) {
        return levels;
    }
    for (std::size_t n = 0; n < levels.size(); ++n) {
        const float relative = std::round(luma[n] / brightest * 255.f);
        levels[n] = static_cast<std::uint8_t>(std::min(static_cast<float>(n), relative));
    }
    return levels;
}

}  // namespace geiss
