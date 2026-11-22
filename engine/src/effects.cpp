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

#include "geiss/effects.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace geiss {
namespace {

// floatframe advanced 1.6 per frame at 30 fps.
constexpr double kFloatFramesPerFrame = 1.6;
// The original's effect sizes are for a 640-wide frame.
constexpr float kReferenceWidth = 640.f;
// How far apart in time the bar's three channels are drawn, as in the original's 4.26.
constexpr double kChromaticDispersion = 4.0;

// The original's seven live effects. Spectral dots and crystals come after them.
constexpr std::size_t kOriginalEffects = 7;
// This port's weight for spectral dots and crystals on every map. The original gave them
// none: spectral dots had weight 0, and crystals were commented out.
constexpr int kAddedEffectWeight = 100;

struct BaseMode {
    // Chasers, Bar, Dots, Solar, Grid, Nuclide, Shade.
    std::array<int, kOriginalEffects> weights;
    int solarMax;
    float centerDwindle;
    int minEffects;
    int maxEffects;
};

// CModeInfo for modes 1...16, before the truecolor adjustment. Mode 8's shade weight is
// 60 because the original chained its assignment into solar_max. Mode 1's solar strength is
// the truecolor one; the 8-bit table had 400.
constexpr std::array<BaseMode, 16> kLowModes = {{
    {{220, 150, 10, 680, 4, 170, 400}, 800, 1.f, 1, 2},
    {{750, 500, 750, 750, 0, 0, 0}, 35, 1.f, 1, 5},
    {{100, 100, 100, 500, 10, 0, 300}, 60, 0.99f, 1, 2},
    {{500, 100, 100, 100, 30, 0, 0}, 34, 0.98f, 1, 2},
    {{100, 350, 100, 500, 15, 180, 500}, 60, 0.99f, 1, 2},
    {{400, 120, 200, 0, 0, 0, 0}, 60, 1.f, 1, 2},
    {{50, 200, 0, 300, 0, 600, 350}, 65, 0.985f, 1, 2},
    {{150, 150, 150, 150, 25, 0, 60}, 60, 0.96f, 1, 2},
    {{450, 200, 50, 200, 0, 100, 200}, 50, 0.985f, 1, 2},
    {{150, 20, 80, 0, 0, 80, 0}, 0, 1.f, 0, 2},
    {{360, 200, 230, 550, 10, 330, 150}, 750, 1.f, 0, 4},
    {{360, 200, 230, 0, 0, 330, 0}, 500, 0.915f, 0, 2},
    {{500, 0, 100, 0, 30, 0, 0}, 34, 0.98f, 1, 2},
    {{500, 0, 100, 0, 30, 0, 0}, 34, 0.98f, 1, 2},
    {{0, 0, 0, 0, 0, 200, 0}, 60, 1.f, 0, 1},
    {{500, 100, 100, 100, 30, 0, 0}, 34, 0.98f, 1, 2},
}};

// Modes 17 and up share one setup. Modes 20...23 darken their center.
constexpr BaseMode kHighMode = {{150, 150, 150, 150, 12, 0, 50}, 600, 1.f, 1, 3};

std::size_t slot(Effect effect) { return static_cast<std::size_t>(effect); }

int capped(int value, int limit) { return std::clamp(value, 0, limit); }

// The pixel's red, green, and blue bytes, or null off the frame.
std::uint8_t* pixel(std::uint8_t* pixels, int width, int height, int x, int y) {
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return nullptr;
    }
    return &pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * kPixelBytes];
}

void addTo(std::uint8_t& channel, int amount) {
    channel = static_cast<std::uint8_t>(std::clamp(channel + amount, 0, 255));
}

// Adds to each channel, saturating at 255.
void add(std::uint8_t* pixels, int width, int height, int x, int y, Rgb amount) {
    if (std::uint8_t* p = pixel(pixels, width, height, x, y)) {
        addTo(p[0], amount.r);
        addTo(p[1], amount.g);
        addTo(p[2], amount.b);
    }
}

// Adds to one channel (0 red, 1 green, 2 blue) only while it is below `limit`, as ShadeBobs
// and Solid_Line did.
void addBelow(std::uint8_t* pixels, int width, int height, int x, int y, int channel, int amount, int limit) {
    if (std::uint8_t* p = pixel(pixels, width, height, x, y); p != nullptr && p[channel] < limit) {
        addTo(p[channel], amount);
    }
}

// Raises each channel to at least `value`.
void raiseTo(std::uint8_t* p, int value) {
    for (int c = 0; c < 3; ++c) {
        if (p[c] < value) {
            p[c] = static_cast<std::uint8_t>(value);
        }
    }
}

bool hasSound(const AudioFrame& audio) { return !audio.silent && !audio.samples.empty(); }

}  // namespace

const char* effectName(Effect effect) {
    static constexpr std::array<const char*, kEffectCount> kNames = {
        "Chasers", "Bar", "Dots", "Solar", "Grid", "Nuclide", "Shade bobs", "Spectral dots", "Crystals",
    };
    const auto index = static_cast<std::size_t>(effect);
    if (index >= kNames.size()) {
        return "?";
    }
    return kNames.at(index);
}

int EffectSet::count() const {
    return static_cast<int>(std::count(on.begin(), on.end(), true));
}

ModeEffects modeEffects(int mode) {
    const BaseMode& base =
        mode >= 1 && mode <= static_cast<int>(kLowModes.size()) ? kLowModes[static_cast<std::size_t>(mode - 1)] : kHighMode;
    ModeEffects info;
    std::copy(base.weights.begin(), base.weights.end(), info.weights.begin());
    info.weights[slot(Effect::Spectral)] = kAddedEffectWeight;
    info.weights[slot(Effect::Crystals)] = kAddedEffectWeight;
    info.solarMax = base.solarMax;
    info.centerDwindle = base.centerDwindle;
    if (mode >= 20 && mode <= 23) {
        info.centerDwindle = 0.98f;
    }
    info.minEffects = base.minEffects;
    info.maxEffects = base.maxEffects;
    // The original's truecolor adjustment: more nuclide, bar, dots, and shade, fewer chasers.
    auto& w = info.weights;
    w[slot(Effect::Nuclide)] = capped(static_cast<int>(static_cast<float>(w[slot(Effect::Nuclide)]) * 1.3f), 900);
    w[slot(Effect::Chasers)] = capped(w[slot(Effect::Chasers)] - 50, 900);
    w[slot(Effect::Dots)] = capped(w[slot(Effect::Dots)] + 220, 900);
    w[slot(Effect::Bar)] = capped(w[slot(Effect::Bar)] + 220, 900);
    w[slot(Effect::Shade)] = capped(w[slot(Effect::Shade)] + 150, 900);
    // And for every color depth, a little more grid.
    w[slot(Effect::Grid)] = capped(w[slot(Effect::Grid)] + 8, 1000);
    return info;
}

EffectSet randomEffects(int mode, bool soundActive, Random& random) {
    const ModeEffects info = modeEffects(mode);
    const auto& weights = info.weights;
    EffectSet effects;
    for (std::size_t i = 0; i < effects.on.size(); ++i) {
        int threshold = weights[i];
        if (soundActive) {
            threshold = static_cast<int>(static_cast<float>(threshold) * 0.7f);
        }
        effects.on[i] = random.below(1000) < threshold;
    }

    // Clip_Num_Effects. Without sound it adds effects until the mode's minimum is met. It
    // stops when no effect left can come up, where the original would have hung.
    int count = effects.count();
    auto canAdd = [&] {
        for (std::size_t i = 0; i < effects.on.size(); ++i) {
            if (!effects.on[i] && weights[i] > 0) {
                return true;
            }
        }
        return false;
    };
    if (!soundActive) {
        while (count < info.minEffects && canAdd()) {
            for (std::size_t i = 0; i < effects.on.size(); ++i) {
                if (!effects.on[i] && random.below(1000) < weights[i]) {
                    effects.on[i] = true;
                    ++count;
                    break;
                }
            }
        }
    }
    for (std::size_t i = 0; i < effects.on.size(); ++i) {
        if (weights[i] >= 1000 && !effects.on[i]) {
            effects.on[i] = true;
            ++count;
        }
    }
    auto canDrop = [&] {
        for (std::size_t i = 0; i < effects.on.size(); ++i) {
            if (effects.on[i] && weights[i] < 1000) {
                return true;
            }
        }
        return false;
    };
    while (count > info.maxEffects && canDrop()) {
        const auto i = static_cast<std::size_t>(random.below(kEffectCount));
        if (effects.on[i] && weights[i] < 1000) {
            effects.on[i] = false;
            --count;
        }
    }

    if (effects.has(Effect::Chasers)) {
        effects.chasers = 1 + random.below(2);
    }
    // A grid turns the heavy bar off.
    if (effects.has(Effect::Grid)) {
        effects.set(Effect::Bar, false);
    }
    effects.solarBurst = mode == 1 && random.below(2) == 0;
    effects.gridDirection = random.below(2) * 2 - 1;
    // The nuclide replaces the waveform 4 times in 7. The original set mode 10's waveform
    // again afterwards, so mode 10 keeps it.
    effects.hideWaveform = effects.has(Effect::Nuclide) && random.below(7) > 2 && mode != 10;
    // Spectral dots always replace the waveform. The original set that last, so mode 10 too.
    effects.hideWaveform = effects.hideWaveform || effects.has(Effect::Spectral);
    return effects;
}

EffectLayer::EffectLayer(std::uint32_t seed) {
    random_.state = seed == 0 ? 1u : seed;
    chaserOffset_ = random_.below(40000);
    for (std::size_t i = 0; i < bobFrequency_.size(); ++i) {
        bobFrequency_[i] = 0.1f + 0.05f * random_.unit();
        bobRadius_[i] = 2.f + 2.8f * random_.unit();
    }
    // micro_c1...c3: how fast each of the bob's channels switches on and off.
    for (float& rate : bobColorRate_) {
        rate = 0.08f + 0.09f * random_.unit();
    }
}

void EffectLayer::clearTrail() { trailFilled_ = 0; }

void EffectLayer::startLook(const EffectSet& effects) {
    clearTrail();
    burstPending_ = effects.solarBurst;
    // A new look's spectral dots start from its own sound, not the last spectral look's.
    spectralLast_ = {};
    // Each dot takes a random color whose channels add up to at least 1.2, as the original's
    // commented-out g_DotColor pick did with each new map.
    for (Tint& color : spectralColor_) {
        do {
            color = {random_.unit(), random_.unit(), random_.unit()};
        } while (color.r + color.g + color.b < 1.2f);
    }
}

void EffectLayer::draw(std::uint8_t* pixels, int width, int height, const EffectSet& effects, int mode,
                       float centerX, float centerY, double frames, const AudioFrame& audio) {
    if (pixels == nullptr || width < 2 || height < 2 || frames <= 0) {
        return;
    }
    const double previous = clock_;
    clock_ += frames;
    const int steps = static_cast<int>(std::floor(clock_) - std::floor(previous));
    const Canvas canvas{pixels, width, height, centerX, centerY, static_cast<float>(width) / kReferenceWidth};
    const ModeEffects info = modeEffects(mode);

    // avg_vol_narrow follows the volume every frame, whether or not the nuclide is on.
    for (int i = 0; i < steps; ++i) {
        narrowVolume_ = narrowVolume_ * 0.3f + audio.volume * 0.7f;
    }

    // Silence keeps the picture calm: no other effect draws until sound returns, and a look
    // that opens in silence loses its solar burst. The nuclide drops its random rings, and the
    // center still darkens, as part of the map.
    if (!hasSound(audio)) {
        burstPending_ = false;
        spectralLast_ = {};
        if (effects.has(Effect::Nuclide)) {
            silentNuclide(canvas, steps);
        }
        diminishCenter(canvas, mode, info.centerDwindle, steps);
        return;
    }
    if (burstPending_) {
        burstPending_ = false;
        // Drop_Solar_Particles(500). The 8-bit original dropped 4 times as many, each fainter.
        dropSolarParticles(canvas, static_cast<int>(500.f * canvas.scale * canvas.scale));
    }
    if (effects.has(Effect::Shade)) {
        shadeBobs(canvas, frames);
    }
    if (effects.has(Effect::Chasers)) {
        chasers(canvas, effects.chasers, previous);
    }
    if (effects.has(Effect::Bar)) {
        bar(canvas, frames);
    }
    if (effects.has(Effect::Dots)) {
        dots(canvas, steps);
    }
    if (effects.has(Effect::Nuclide)) {
        nuclide(canvas, steps, audio);
    }
    if (effects.has(Effect::Spectral)) {
        spectral(canvas, steps, frames, audio);
    }
    if (modeHasEdges(mode) && steps > 0) {
        edges(canvas);
    }
    if (effects.has(Effect::Grid)) {
        grid(canvas, effects.gridDirection);
    }
    if (effects.has(Effect::Crystals)) {
        crystals(canvas, steps);
    }
    if (effects.has(Effect::Solar)) {
        solar(canvas, info.solarMax, frames);
    }
    diminishCenter(canvas, mode, info.centerDwindle, steps);
}

// ShadeBobs: one bob wobbles a few pixels around the center and takes four jittered
// steps per frame. In truecolor each of its channels switches on while sin(floatframe *
// micro_c) is between 0 and 1, about half the time, so its color drifts; a channel that is on
// adds 5 to the pixel and 3 to the four beside it, while below 250.
void EffectLayer::shadeBobs(const Canvas& canvas, double frames) {
    bobSteps_ += 4.0 * frames;
    const auto t = static_cast<float>(clock_ * kFloatFramesPerFrame);
    const float s = canvas.scale;
    int a = static_cast<int>(canvas.centerX + s * (bobRadius_[0] * std::cos(t * bobFrequency_[0]) +
                                                    bobRadius_[2] * std::cos(t * bobFrequency_[1])));
    int b = static_cast<int>(canvas.centerY + s * (bobRadius_[1] * std::cos(t * bobFrequency_[2]) +
                                                    bobRadius_[3] * std::cos(t * bobFrequency_[3])));
    // The original's cr went to its first byte, which is blue, and cb to red.
    std::array<bool, 3> on{};
    for (std::size_t c = 0; c < on.size(); ++c) {
        on.at(2 - c) = static_cast<int>(1.f + std::sin(t * bobColorRate_.at(c))) == 1;
    }
    while (bobSteps_ >= 1.0) {
        bobSteps_ -= 1.0;
        b += random_.below(5) - 2;
        a += random_.below(5) - 2;
        for (int c = 0; c < 3; ++c) {
            if (!on.at(static_cast<std::size_t>(c))) {
                continue;
            }
            addBelow(canvas.pixels, canvas.width, canvas.height, a, b, c, 5, 250);
            addBelow(canvas.pixels, canvas.width, canvas.height, a + 1, b, c, 3, 250);
            addBelow(canvas.pixels, canvas.width, canvas.height, a - 1, b, c, 3, 250);
            addBelow(canvas.pixels, canvas.width, canvas.height, a, b + 1, c, 3, 250);
            addBelow(canvas.pixels, canvas.width, canvas.height, a, b - 1, c, 3, 250);
        }
    }
}

// Two_Chasers: one or two points on two-cosine curves around the center. Each frame they
// move 1.6 floatframes in 20 steps (at 640 wide), and each step lifts every channel of its
// pixel 40% of the way to 255, so they draw white. The steps sit on a fixed grid of floatframes, so the path is the same at any
// frame rate.
void EffectLayer::chasers(const Canvas& canvas, int count, double previousClock) {
    const float s = canvas.scale;
    const int perFrame = std::max(1, static_cast<int>(20.f * s));
    const double spacing = kFloatFramesPerFrame / perFrame;
    const double start = previousClock * kFloatFramesPerFrame + chaserOffset_;
    const double end = clock_ * kFloatFramesPerFrame + chaserOffset_;
    // Steps are counted by index, so a step on a tick boundary is drawn exactly once.
    const auto last = static_cast<long long>(std::floor(end / spacing));
    for (auto n = static_cast<long long>(std::floor(start / spacing)) + 1; n <= last; ++n) {
        const double t = static_cast<double>(n) * spacing;
        for (int pass = 0; pass < count; ++pass) {
            int a = 0;
            int b = 0;
            if (pass == 0) {
                a = static_cast<int>(canvas.centerX + s * static_cast<float>(74.0 * std::cos(t * 0.1102 + 10.0) +
                                                                             65.0 * std::cos(t * 0.1312 + 20.0)));
                b = static_cast<int>(canvas.centerY + s * static_cast<float>(54.0 * std::cos(t * 0.1204 + 40.0) +
                                                                             55.0 * std::cos(t * 0.1715 + 30.0)));
            } else {
                a = static_cast<int>(canvas.centerX + s * static_cast<float>(64.0 * std::cos(t * 0.1213 + 33.0) +
                                                                             55.0 * std::cos(t * 0.1408 + 15.0)));
                b = static_cast<int>(canvas.centerY + s * static_cast<float>(52.0 * std::cos(t * 0.1304 + 12.0) +
                                                                             51.0 * std::cos(t * 0.1103 + 21.0)));
            }
            if (std::uint8_t* p = pixel(canvas.pixels, canvas.width, canvas.height, a, b)) {
                for (int c = 0; c < 3; ++c) {
                    p[c] = static_cast<std::uint8_t>(255.f - static_cast<float>(255 - p[c]) * 0.6f);
                }
            }
        }
    }
}

// Solid_Line: a short line between two endpoints that wander near the center. Each frame
// adds 16 to every pixel on it that is below 223. Between frames it adds its share of 16.
// In truecolor the original drew it once per channel, green and red a little ahead of and
// behind blue in time (chromatic_dispersion 4), so it fringes in color as it moves.
void EffectLayer::bar(const Canvas& canvas, double frames) {
    barCarry_ += static_cast<float>(16.0 * frames);
    const int amount = static_cast<int>(barCarry_);
    if (amount < 1) {
        return;
    }
    barCarry_ -= static_cast<float>(amount);
    const double floatframe = clock_ * kFloatFramesPerFrame;
    const std::array<double, 3> lead = {
        -3.5 * kChromaticDispersion * (std::cos(floatframe * 0.05 + 2) + std::sin(floatframe * 0.06 + 4)),
        3.5 * kChromaticDispersion * (std::sin(floatframe * 0.03 + 1) + std::cos(floatframe * 0.04 + 3)),
        0.0,
    };
    for (int c = 0; c < 3; ++c) {
        barLine(canvas, floatframe + lead.at(static_cast<std::size_t>(c)), c, amount);
    }
}

void EffectLayer::barLine(const Canvas& canvas, double floatframe, int channel, int amount) {
    const float s = canvas.scale;
    // The bar uses 0.6 of the chaser offset, and 0.55 / 1.6 of floatframe.
    const double t = (floatframe + chaserOffset_ * 0.6) * 0.55 / kFloatFramesPerFrame;
    const float x1 = canvas.centerX + s * static_cast<float>(16.0 * std::cos(t * 0.1102 + 10) + 15.0 * std::cos(t * 0.1312 + 20));
    const float y1 = canvas.centerY + s * static_cast<float>(15.0 * std::cos(t * 0.1204 + 40) + 10.0 * std::cos(t * 0.1715 + 30));
    const float x2 = canvas.centerX + s * static_cast<float>(14.0 * std::cos(t * 0.1213 + 33) + 13.0 * std::cos(t * 0.1408 + 15));
    const float y2 = canvas.centerY + s * static_cast<float>(13.0 * std::cos(t * 0.1304 + 12) + 11.0 * std::cos(t * 0.1103 + 21));
    const int points = std::max(1, static_cast<int>(s * 50.f));
    for (int k = 0; k < points; ++k) {
        const float u = static_cast<float>(k) / static_cast<float>(points);
        const int a = static_cast<int>(x1 * u + x2 * (1.f - u));
        const int b = static_cast<int>(y1 * u + y2 * (1.f - u));
        addBelow(canvas.pixels, canvas.width, canvas.height, a, b, channel, amount, 223);
    }
}

// One_Dotty_Chaser: a slow point leaves a trail of its last 20 positions. Each frame the
// trail is redrawn in a color whose channels cycle at three rates, and every dot drifts one
// pixel right. From 880 pixels wide each dot is 2x2, as the original's truecolor dots were.
void EffectLayer::dots(const Canvas& canvas, int steps) {
    const float s = canvas.scale;
    const int size = canvas.width >= 880 ? 2 : 1;
    for (int step = 0; step < std::min(steps, kTrail); ++step) {
        const double t = (clock_ - static_cast<double>(std::min(steps, kTrail) - 1 - step)) * kFloatFramesPerFrame;
        const int a = static_cast<int>(canvas.centerX + s * static_cast<float>(64.0 * std::cos(t * 0.0613 + 33) +
                                                                               55.0 * std::cos(t * 0.0708 + 15)));
        const int b = static_cast<int>(canvas.centerY + s * static_cast<float>(52.0 * std::cos(t * 0.0704 + 12) +
                                                                               51.0 * std::cos(t * 0.0503 + 21)));
        if (b < 0 || b >= canvas.height) {
            continue;
        }
        trailNewest_ = (trailNewest_ + 1) % kTrail;
        trailFilled_ = std::min(trailFilled_ + 1, kTrail);
        const auto newest = static_cast<std::size_t>(trailNewest_);
        trailX_[newest] = a;
        trailY_[newest] = b;
        // The original's chaser_r went to its first byte, which is blue, and chaser_b to red.
        trailColor_[newest] = {static_cast<int>(127.0 + 126.0 * std::sin(t * 0.0513 + 27)),
                               static_cast<int>(127.0 + 126.0 * std::sin(t * 0.0713 + 30)),
                               static_cast<int>(127.0 + 126.0 * std::sin(t * 0.0613 + 33))};
        for (int k = 0; k < trailFilled_; ++k) {
            const auto i = static_cast<std::size_t>((trailNewest_ - k + kTrail) % kTrail);
            for (int dy = 0; dy < size; ++dy) {
                for (int dx = 0; dx < size; ++dx) {
                    if (std::uint8_t* p = pixel(canvas.pixels, canvas.width, canvas.height, trailX_[i] + dx, trailY_[i] + dy)) {
                        p[0] = static_cast<std::uint8_t>(trailColor_[i].r);
                        p[1] = static_cast<std::uint8_t>(trailColor_[i].g);
                        p[2] = static_cast<std::uint8_t>(trailColor_[i].b);
                    }
                }
            }
            ++trailX_[i];
        }
    }
}

// The solar flare: Drop_Solar_Particles' count swings with time and the mode's strength.
// At 640 wide that is int(fx * 0.05) particles a frame; the 8-bit original dropped 4 times as
// many. Wider frames get more, so the
// density stays the same.
void EffectLayer::solar(const Canvas& canvas, int solarMax, double frames) {
    const double f = clock_;
    const double max = solarMax;
    const double fx = 3.0 + max * 1.6 + max * 0.43 * std::sin(f * 0.05) + max * 0.43 * std::sin(f * 0.038 + 1);
    const int perFrame = static_cast<int>(fx * 0.05);
    solarParticles_ += static_cast<double>(perFrame) * frames * static_cast<double>(canvas.scale * canvas.scale);
    const int count = static_cast<int>(solarParticles_);
    solarParticles_ -= count;
    dropSolarParticles(canvas, count);
}

// Each particle lands within 35 pixels (at 640 wide) of the center and adds a 3x3 glow that
// is brighter near the center. In truecolor each channel takes its own random strength,
// 4 + rand()%30 * (35 - distance) / 25, so the particles sparkle in many colors, and a
// channel already at 207 minus that strength takes none.
void EffectLayer::dropSolarParticles(const Canvas& canvas, int count) {
    for (int k = 0; k < count; ++k) {
        int x = 0;
        int y = 0;
        float distance = 100.f;
        while (distance >= 35.f) {
            y = random_.below(72) - 36;
            x = random_.below(96) - 48;
            distance = std::sqrt(static_cast<float>(x * x + y * y));
        }
        const int a = static_cast<int>(canvas.centerX + static_cast<float>(x) * canvas.scale);
        const int b = static_cast<int>(canvas.centerY + static_cast<float>(y) * canvas.scale);
        const std::uint8_t* center = pixel(canvas.pixels, canvas.width, canvas.height, a, b);
        if (center == nullptr) {
            continue;
        }
        // The original's first byte was blue, so its channels came up blue, green, red.
        for (int c = 2; c >= 0; --c) {
            const int i0 = 4 + static_cast<int>(static_cast<float>(random_.below(30)) * (35.f - distance) / 25.f);
            const int i1 = i0 - 3;
            const int i2 = i1 / 2;
            if (center[c] >= 207 - i0) {
                continue;
            }
            Rgb strong{};
            Rgb side{};
            Rgb corner{};
            (c == 0 ? strong.r : c == 1 ? strong.g : strong.b) = i0;
            (c == 0 ? side.r : c == 1 ? side.g : side.b) = i1;
            (c == 0 ? corner.r : c == 1 ? corner.g : corner.b) = i2;
            add(canvas.pixels, canvas.width, canvas.height, a, b, strong);
            add(canvas.pixels, canvas.width, canvas.height, a + 1, b, side);
            add(canvas.pixels, canvas.width, canvas.height, a - 1, b, side);
            add(canvas.pixels, canvas.width, canvas.height, a, b + 1, side);
            add(canvas.pixels, canvas.width, canvas.height, a, b - 1, side);
            add(canvas.pixels, canvas.width, canvas.height, a - 1, b - 1, corner);
            add(canvas.pixels, canvas.width, canvas.height, a + 1, b - 1, corner);
            add(canvas.pixels, canvas.width, canvas.height, a - 1, b + 1, corner);
            add(canvas.pixels, canvas.width, canvas.height, a + 1, b + 1, corner);
        }
    }
}

// Grid: a dot every 1/30 of the width, in both directions. The dots scroll one pixel a
// frame, left or right, and their brightness pulses. A dot never darkens a channel, so it
// draws grey over dark and leaves brighter colors alone.
void EffectLayer::grid(const Canvas& canvas, int direction) {
    const int spacing = std::max(2, canvas.width / 30);
    const double f = clock_;
    const double level = 65.0 + 45.0 * std::sin(f * 0.06033) + 35.0 * std::cos(f * 0.04710 + 1) +
                         25.0 * std::cos(f * 0.00523 - 1);
    const int bright = std::clamp(static_cast<int>(level), 0, 255);
    if (bright == 0) {
        return;
    }
    int shift = static_cast<int>(static_cast<long long>(std::floor(f)) % spacing);
    if (direction == 1) {
        shift = -shift;
    }
    // One column past the edge, so a column scrolling in from the right is drawn before it
    // reaches its slot. The original reached it by writing into the next row.
    for (int y = 0; y < canvas.height; y += spacing) {
        for (int x = 0; x < canvas.width + spacing; x += spacing) {
            if (std::uint8_t* p = pixel(canvas.pixels, canvas.width, canvas.height, x + shift, y)) {
                raiseTo(p, bright);
            }
        }
    }
}

// Nuclide: a jump in volume (above 1.1 times avg_vol_narrow) drops a ring of discs whose
// size follows the jump.
void EffectLayer::nuclide(const Canvas& canvas, int steps, const AudioFrame& audio) {
    for (int step = 0; step < steps; ++step) {
        if (audio.volume > narrowVolume_ * 1.1f) {
            const int radius = std::clamp(static_cast<int>(3.f + 40.f * (audio.volume / narrowVolume_ - 1.1f)), 1, 10);
            nuclideRing(canvas, static_cast<float>(radius), nuclideTint(clock_, drift_));
        }
    }
}

// In silence, a ring of random size drops about one frame in 12, as the original did.
void EffectLayer::silentNuclide(const Canvas& canvas, int steps) {
    for (int step = 0; step < steps; ++step) {
        if (random_.below(12) == 0) {
            nuclideRing(canvas, static_cast<float>(3 + random_.below(8)), silentNuclideTint(clock_, drift_));
        }
    }
}

// 3...7 soft discs evenly around a circle of 34...41 pixels (at 640 wide) about the center,
// tinted by the color drift.
void EffectLayer::nuclideRing(const Canvas& canvas, float discRadius, Tint tint) {
    const int nodes = 3 + random_.below(5);
    const auto phase = static_cast<float>(random_.below(1000));
    const float ring = static_cast<float>(34 + random_.below(8)) * canvas.scale;
    for (int n = 0; n < nodes; ++n) {
        const float angle = static_cast<float>(n) / static_cast<float>(nodes) * 6.28f + phase;
        disc(canvas, static_cast<int>(canvas.centerX + ring * std::cos(angle)),
             static_cast<int>(canvas.centerY + ring * std::sin(angle)), discRadius, 1.f, tint);
    }
}

// A soft disc of up to 10 pixels (at 640 wide). It adds (radius - distance) * 25 * strength,
// times each channel's tint, out to its radius.
void EffectLayer::disc(const Canvas& canvas, int cx, int cy, float radius, float strength, Tint tint) {
    const int extent = static_cast<int>(std::ceil(10.f * canvas.scale));
    for (int dy = -extent; dy <= extent; ++dy) {
        for (int dx = -extent; dx <= extent; ++dx) {
            const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy)) / canvas.scale;
            const float value = (radius - distance) * 25.f * strength;
            if (value > 0.f) {
                add(canvas.pixels, canvas.width, canvas.height, cx + dx, cy + dy,
                    {static_cast<int>(value * tint.r), static_cast<int>(value * tint.g), static_cast<int>(value * tint.b)});
            }
        }
    }
}

// Spectral dots: one soft disc for each of bass, middle, and treble, each in its own color. Each band's level is
// smoothed (0.6 new, 0.4 old per frame) and sets its disc's radius, level * 0.06 - 2, up to 10.
// A disc wanders on a two-cosine path around the center. As in the original, the path's phase
// moves whenever the level rises or falls by less than 4: back by 2.5 per level it rises, and
// forward a little on a slight fall, so a steep fall holds the disc where it is.
//
// The levels and phases advance once per reference frame. Faster than that, each display frame
// stamps the discs at their current place with its share of a frame, as the bar does, so the
// map smears them into a continuous streak at the same brightness as at 30 fps.
void EffectLayer::spectral(const Canvas& canvas, int steps, double frames, const AudioFrame& audio) {
    if (frames < 1.0) {
        for (int step = 0; step < steps; ++step) {
            advanceSpectral(audio);
        }
        drawSpectral(canvas, static_cast<float>(frames));
        return;
    }
    for (int step = 0; step < steps; ++step) {
        advanceSpectral(audio);
        drawSpectral(canvas, 1.f);
    }
}

void EffectLayer::advanceSpectral(const AudioFrame& audio) {
    for (std::size_t band = 0; band < spectralLast_.size(); ++band) {
        const float last = spectralLast_.at(band);
        const float level = std::clamp(audio.spectrum.at(band) * 0.6f + last * 0.4f, 0.f, 255.f);
        if (level > last - 4.f) {
            spectralPhase_.at(band) += (last - level) * 2.5f;
        }
        spectralLast_.at(band) = level;
    }
}

void EffectLayer::drawSpectral(const Canvas& canvas, float strength) {
    const float s = canvas.scale;
    for (std::size_t band = 0; band < spectralLast_.size(); ++band) {
        const float radius = std::min(spectralLast_.at(band) * 0.06f - 2.f, 10.f);
        if (radius <= 0.f) {
            continue;
        }
        const auto n = static_cast<float>(band);
        const float p = spectralPhase_.at(band);
        const float x = 64.f * std::cos(p * 0.00451f + n * 99.f + 3.f) + 51.f * std::cos(p * 0.00572f + n * 13.f + 7.f);
        const float y = 51.f * std::cos(p * 0.00502f + n * 78.f + 8.f) + 45.f * std::cos(p * 0.00653f + n * 17.f + 5.f);
        disc(canvas, static_cast<int>(canvas.centerX + s * x), static_cast<int>(canvas.centerY + s * y), radius, strength,
             spectralColor_.at(band));
    }
}

// Crystals: each frame, 2...7 spokes of 6...25 pixels (at 640 wide) burst from a point within
// 15 pixels of the center. Every channel of every pixel they cross is raised to at least 150.
void EffectLayer::crystals(const Canvas& canvas, int steps) {
    const float s = canvas.scale;
    for (int step = 0; step < steps; ++step) {
        const float cx = canvas.centerX + static_cast<float>(random_.below(30) - 15) * s;
        const float cy = canvas.centerY + static_cast<float>(random_.below(30) - 15) * s;
        const int spokes = 2 + random_.below(6);
        const float length = static_cast<float>(6 + random_.below(20)) * s;
        const auto phase = static_cast<float>(random_.below(100));
        for (int n = 0; n < spokes; ++n) {
            const float angle = static_cast<float>(n) / static_cast<float>(spokes) * 6.28f + phase;
            const float dx = std::cos(angle);
            const float dy = std::sin(angle);
            const auto halfSteps = static_cast<int>(std::ceil(length * 2.f));
            for (int k = 0; k < halfSteps; ++k) {
                const float t = static_cast<float>(k) * 0.5f;
                if (std::uint8_t* p = pixel(canvas.pixels, canvas.width, canvas.height, static_cast<int>(cx + dx * t),
                                            static_cast<int>(cy + dy * t))) {
                    raiseTo(p, 150);
                }
            }
        }
    }
}

// Mode6Edges: three rows along the top and three along the bottom take noise of 0...39 over a
// slow wave, so the vortex field pulls a moving texture in from both edges. In truecolor each
// channel has its own wave of 0...198, as in the original, so the texture shifts in color and
// its peaks show at full brightness and glow on an HDR screen.
// The rows are overwritten once per reference frame, so the texture is the same at any refresh.
void EffectLayer::edges(const Canvas& canvas) {
    const int rows = std::min(3, canvas.height / 2);
    const double f = clock_;
    // Per channel, red, green, blue: the bottom wave's and the top wave's time and space rates.
    // The original's first byte, blue, took the first pair.
    struct Wave {
        double bottomTime;
        double bottomSpace;
        double topTime;
        double topSpace;
    };
    static constexpr std::array<Wave, 3> kWaves = {{
        {0.031, 0.033, -0.035, 0.029},
        {0.037, 0.035, -0.032, 0.028},
        {0.040, 0.031, -0.030, 0.024},
    }};
    for (int x = 0; x < canvas.width; ++x) {
        const double u = static_cast<double>(x) / static_cast<double>(canvas.scale);
        std::array<int, 3> bottom{};
        std::array<int, 3> top{};
        for (std::size_t c = 0; c < kWaves.size(); ++c) {
            bottom.at(c) = static_cast<int>(99.0 + 99.0 * std::sin(f * kWaves.at(c).bottomTime + u * kWaves.at(c).bottomSpace));
            top.at(c) = static_cast<int>(99.0 + 99.0 * std::sin(f * kWaves.at(c).topTime + u * kWaves.at(c).topSpace));
        }
        for (int r = 0; r < rows; ++r) {
            if (std::uint8_t* p = pixel(canvas.pixels, canvas.width, canvas.height, x, r)) {
                for (std::size_t c = 0; c < 3; ++c) {
                    p[c] = static_cast<std::uint8_t>(top.at(c) + random_.below(40));
                }
            }
            if (std::uint8_t* p = pixel(canvas.pixels, canvas.width, canvas.height, x, canvas.height - 1 - r)) {
                for (std::size_t c = 0; c < 3; ++c) {
                    p[c] = static_cast<std::uint8_t>(bottom.at(c) + random_.below(40));
                }
            }
        }
    }
}

// Diminish_Center: darkens each channel of the center pixel and its four neighbors each frame, so the
// zoom does not burn a hot spot there. Mode 12 splits sideways from a vertical line, so
// it darkens a 3-pixel column instead.
void EffectLayer::diminishCenter(const Canvas& canvas, int mode, float dwindle, int steps) {
    if (dwindle >= 0.999f || steps < 1) {
        return;
    }
    const auto factor = static_cast<float>(std::pow(static_cast<double>(dwindle), steps));
    const int cx = static_cast<int>(canvas.centerX);
    const int cy = static_cast<int>(canvas.centerY);
    auto darken = [&](int x, int y, int floor) {
        if (std::uint8_t* p = pixel(canvas.pixels, canvas.width, canvas.height, x, y)) {
            for (int c = 0; c < 3; ++c) {
                if (p[c] > floor) {
                    p[c] = static_cast<std::uint8_t>(static_cast<float>(p[c]) * factor);
                }
            }
        }
    };
    if (mode == 12) {
        for (int y = 0; y < canvas.height; ++y) {
            darken(cx - 1, y, 0);
            darken(cx, y, 0);
            darken(cx + 1, y, 0);
        }
        return;
    }
    darken(cx, cy, 1);
    darken(cx - 1, cy, 1);
    darken(cx + 1, cy, 1);
    darken(cx, cy + 1, 1);
    darken(cx, cy - 1, 1);
}

}  // namespace geiss
