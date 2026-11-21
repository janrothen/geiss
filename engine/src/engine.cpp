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

#include "geiss/engine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>

namespace geiss {
namespace {

constexpr double kReferenceFps = 30.0;
constexpr double kMaxDt = 0.1;
constexpr double kMapInterval = 20.0;
// PutPalette blended to a new palette over 18 steps, counted down twice per frame:
// about 9 frames, or 0.3 s at 30 fps.
constexpr double kPaletteBlendSeconds = 0.3;
// The original's SoundEmpty: true after frames_til_auto_switch * 2 (1100) frames of silence
// at 30 fps. Only then does a new palette get the brighter silent gamma, so a short gap
// between tracks, or the first look after launch, keeps the normal gamma.
constexpr double kSoundEmptySeconds = 1100.0 / kReferenceFps;
// When a look is due during beat mode, it waits for a big beat, as the finished map did in
// the original. The threshold starts at 1.10 and relaxes by 0.2.
// Intentional difference: the original relaxed over its 550-frame (about 18 s) switch
// period. Here it relaxes over the 5 s wait, and the look changes when the wait ends.
// A steady beat peaks near 1.05, so it lands the change on a hit after about 1.3 s,
// and a louder hit lands it at once.
constexpr float kBigBeatThreshold = 1.10f;
constexpr float kBigBeatRelax = 0.2f;
constexpr double kMaxBeatWait = 5.0;
constexpr double kBigBeatRelaxSeconds = kMaxBeatWait;

// The idle wave. Intentional difference: the original drew nothing in silence, so the
// picture faded to black. Here, once silence has lasted kIdleWaitSeconds, a made-up signal
// rises over kIdleRiseSeconds and is drawn with the look's own waveform: the line becomes
// a slow swell, two lines braid, the circle opens into a breathing flower, and the
// oscilloscope traces a 3:2 Lissajous figure that slowly turns and morphs. It stays in the
// palette's lower half, so it is calm and never glows on HDR.
constexpr double kIdleWaitSeconds = 1.5;
constexpr double kIdleRiseSeconds = 4.0;
// Palette indices: the wave breathes between about 70 and 130 over kIdleBreathSeconds.
constexpr float kIdleBright = 100.f;
constexpr float kIdleBreath = 30.f;
// The hand-off. Sound whose line would draw dimmer than the idle wave's darkest keeps the
// idle wave, so quiet music never looks darker than silence; its faint line plays inside it.
// Brighter sound fades the idle wave out over kIdleFadeSeconds instead of cutting it, and
// while any of it is left, quiet brings it back up without the wait, so sound hovering
// near the hand-off makes it dip instead of blinking off and on.
constexpr float kIdleHandOff = kIdleBright - kIdleBreath;
constexpr double kIdleFadeSeconds = 0.5;
constexpr double kIdleBreathSeconds = 7.0;
// The signal's size swells between 0.2 and 0.4 of a full-scale wave over kIdleSwellSeconds.
constexpr float kIdleAmplitude = 0.3f;
constexpr float kIdleSwell = 0.1f;
constexpr double kIdleSwellSeconds = 11.0;
constexpr double kTau = 2.0 * std::numbers::pi;

bool lookMayChange(double overdue, const AudioFrame& audio) {
    if (!audio.beatMode || overdue >= kMaxBeatWait) {
        return true;
    }
    const auto threshold = kBigBeatThreshold - kBigBeatRelax * static_cast<float>(overdue / kBigBeatRelaxSeconds);
    return audio.bigBeatRatio > threshold;
}

// A -1...1 sample covers about 35% of the height. The original scaled 16-bit capture
// by volscale. That constant waits until real tap levels are known.
constexpr float kWaveFraction = 0.35f;
// RenderWave waveform 6 uses sin(intframe * 0.01). At the 30 fps reference that is sin(t * 0.3).
constexpr float kScopeAngleRate = 0.3f;
constexpr int kScopePoints = 314;
// RenderWave waveform 5 blends its first 50 points toward the points after 314, so the circle closes.
constexpr int kCirclePoints = 314;
constexpr int kCircleBlend = 50;

float columnSample(const std::vector<float>& samples, int x, int width) {
    if (samples.empty() || width < 2) {
        return 0.f;
    }
    const int clamped = std::clamp(x, 0, width - 1);
    if (static_cast<int>(samples.size()) == 1) {
        return samples[0];
    }
    const std::size_t index = static_cast<std::size_t>(clamped) * (samples.size() - 1) / static_cast<std::size_t>(width - 1);
    return samples[index];
}

// RenderWave's plugin brightness: base = current_vol*4 + avg_vol*0.4 - 10, clamped to
// 0...155. The original then tinted that RGB base up to about 255; here 0...155 maps
// onto the palette's 0...255 index range, so normal listening levels draw at full brightness.
constexpr float kBaseCeiling = 155.f;

float waveBrightness(float volume, float averageVolume) {
    const float base = std::clamp(volume * 4.f + averageVolume * 0.4f - 10.f, 0.f, kBaseCeiling);
    return base * 255.f / kBaseCeiling;
}

// How a waveform's points are drawn: the palette index, and whether each point is a soft
// spot instead of one sharp pixel.
struct Ink {
    int bright = 0;
    bool soft = false;
};

void plotPixel(std::uint8_t* indices, int width, int height, int x, int y, int bright) {
    if (x < 0 || y < 0 || x >= width || y >= height || bright < 1) {
        return;
    }
    std::uint8_t& pixel = indices[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
    if (pixel < static_cast<std::uint8_t>(bright)) {
        pixel = static_cast<std::uint8_t>(bright);
    }
}

// A soft spot's 5x5 falloff, close to a Gaussian with a radius of about one pixel, in
// 1/256ths of the center.
constexpr std::array<int, 25> kSoftSpot = {
    13, 48,  70,  48,  13,
    48, 113, 169, 113, 48,
    70, 169, 256, 169, 70,
    48, 113, 169, 113, 48,
    13, 48,  70,  48,  13,
};

}  // namespace

void plotSoftSpot(std::uint8_t* indices, int width, int height, int x, int y, int bright) {
    if (indices == nullptr) {
        return;
    }
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int weight = kSoftSpot.at((static_cast<std::size_t>(dy + 2) * 5) + static_cast<std::size_t>(dx + 2));
            plotPixel(indices, width, height, x + dx, y + dy, bright * weight / 256);
        }
    }
}

float wrappedPhase(double radiansPerSecond, double seconds) {
    return static_cast<float>(std::fmod(radiansPerSecond * seconds, kTau));
}

namespace {

void plot(std::uint8_t* indices, int width, int height, int x, int y, Ink ink) {
    if (ink.soft) {
        plotSoftSpot(indices, width, height, x, y, ink.bright);
    } else {
        plotPixel(indices, width, height, x, y, ink.bright);
    }
}

// RenderWave waveform 1: one smoothed line, drawn with a max blend.
// It reads the mono columns and stays on the frame center. Over mode 10's scroll the
// original lowered it to three quarters of the height.
void drawLine(std::uint8_t* indices, int width, int height, const AudioFrame& audio, Ink ink, int mode) {
    const float amplitude = static_cast<float>(height) * kWaveFraction;
    const float yCenter = static_cast<float>(height) * (mode == 10 ? 0.75f : 0.5f);
    float previous = columnSample(audio.samples, 0, width) * amplitude + yCenter;
    for (int x = 0; x < width; ++x) {
        const float z = columnSample(audio.samples, x, width) * amplitude + yCenter;
        previous = previous * 0.9f + z * 0.1f;
        plot(indices, width, height, x, static_cast<int>(previous), ink);
    }
}

// RenderWave waveform 2: two smoothed lines. Left sits 12% of the height above the
// map center, right sits 12% below, and both use 0.7 of the line amplitude.
// Intentional difference: each column uses that column's channel. The original
// interleaved buffer repeated each stereo pair across two pixels.
void drawTwoLines(std::uint8_t* indices, int width, int height, const AudioFrame& audio,
                  Ink ink, float centerYNorm) {
    const float amplitude = static_cast<float>(height) * kWaveFraction * 0.7f;
    const float center = centerYNorm * static_cast<float>(height);
    const float upper = center - static_cast<float>(height) * 0.12f;
    const float lower = center + static_cast<float>(height) * 0.12f;
    const std::vector<float>& left = audio.left.empty() ? audio.samples : audio.left;
    const std::vector<float>& right = audio.right.empty() ? left : audio.right;
    float previousLeft = columnSample(left, 0, width) * amplitude + upper;
    float previousRight = columnSample(right, 0, width) * amplitude + lower;
    for (int x = 0; x < width; ++x) {
        previousLeft = previousLeft * 0.9f + (columnSample(left, x, width) * amplitude + upper) * 0.1f;
        previousRight = previousRight * 0.9f + (columnSample(right, x, width) * amplitude + lower) * 0.1f;
        plot(indices, width, height, x, static_cast<int>(previousLeft), ink);
        plot(indices, width, height, x, static_cast<int>(previousRight), ink);
    }
}

// RenderWave waveform 6: an X-Y oscilloscope. 314 smoothed stereo points, scaled by 1.2,
// rocked around the map center. The original seeds the smoother with the unscaled first pair.
// Intentional difference: the original walks the first 314 frames of the capture buffer.
// This frame is already one sample per column, so the 314 points cover that whole wave.
void drawScope(std::uint8_t* indices, int width, int height, const AudioFrame& audio,
               Ink ink, float centerXNorm, float centerYNorm, float motionTime) {
    const float amplitude = static_cast<float>(height) * kWaveFraction * 1.2f;
    const float centerX = centerXNorm * static_cast<float>(width);
    const float centerY = centerYNorm * static_cast<float>(height);
    const float angle = std::sin(motionTime * kScopeAngleRate);
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    const std::vector<float>& left = audio.left.empty() ? audio.samples : audio.left;
    const std::vector<float>& right = audio.right.empty() ? left : audio.right;
    float px2 = columnSample(left, 0, width);
    float py2 = columnSample(right, 0, width);
    for (int i = 0; i < kScopePoints; ++i) {
        const int column = (i * (width - 1)) / (kScopePoints - 1);
        px2 = px2 * 0.5f + 0.5f * columnSample(left, column, width) * amplitude;
        py2 = py2 * 0.5f + 0.5f * columnSample(right, column, width) * amplitude;
        const float px = px2 * cosine + py2 * sine;
        const float py = px2 * -sine + py2 * cosine;
        plot(indices, width, height, static_cast<int>(px + centerX), static_cast<int>(py + centerY), ink);
    }
}

// RenderWave waveform 3: one smoothed vertical line through the map center. Row y reads
// the sample of column y, as the original read its buffer by row.
void drawVerticalLine(std::uint8_t* indices, int width, int height, const AudioFrame& audio,
                      Ink ink, float centerXNorm) {
    const float amplitude = static_cast<float>(height) * kWaveFraction;
    const float xCenter = centerXNorm * static_cast<float>(width);
    float previous = columnSample(audio.samples, 0, width) * amplitude + xCenter;
    for (int y = 0; y < height; ++y) {
        previous = previous * 0.9f + (columnSample(audio.samples, y, width) * amplitude + xCenter) * 0.1f;
        plot(indices, width, height, static_cast<int>(previous), y, ink);
    }
}

// RenderWave waveform 4: two smoothed diagonal lines at 0.9 of the line amplitude. Left
// runs down from the top-left corner and right from width - height, both one pixel
// across per row.
void drawDiagonals(std::uint8_t* indices, int width, int height, const AudioFrame& audio, Ink ink) {
    const float amplitude = static_cast<float>(height) * kWaveFraction * 0.9f;
    const std::vector<float>& left = audio.left.empty() ? audio.samples : audio.left;
    const std::vector<float>& right = audio.right.empty() ? left : audio.right;
    float previousLeft = columnSample(left, 0, width) * amplitude;
    float previousRight = columnSample(right, 0, width) * amplitude;
    for (int y = 0; y < height; ++y) {
        previousLeft = previousLeft * 0.9f + columnSample(left, y, width) * amplitude * 0.1f;
        previousRight = previousRight * 0.9f + columnSample(right, y, width) * amplitude * 0.1f;
        plot(indices, width, height, static_cast<int>(previousLeft) + y, y, ink);
        plot(indices, width, height, static_cast<int>(previousRight) + y + (width - height), y, ink);
    }
}

// RenderWave waveform 5: a circle around the map center whose radius follows the mono
// wave at 0.7 of the line amplitude. The base radius is 60 pixels on a 640-wide frame,
// and points closer than 5 (at 640) to the center are skipped.
// Intentional difference: like waveform 6, the 364 points cover the whole column wave
// instead of the first 364 frames of the capture buffer.
void drawCircle(std::uint8_t* indices, int width, int height, const AudioFrame& audio,
                Ink ink, float centerXNorm, float centerYNorm) {
    const float amplitude = static_cast<float>(height) * kWaveFraction * 0.7f;
    const float toPixels = static_cast<float>(width) / 640.f;
    const float baseRadius = 60.f * toPixels;
    const float centerX = centerXNorm * static_cast<float>(width);
    const float centerY = centerYNorm * static_cast<float>(height);
    constexpr int span = kCirclePoints + kCircleBlend;
    auto sampleAt = [&](int point) {
        return columnSample(audio.samples, point * (width - 1) / (span - 1), width);
    };
    // The original blended its buffer in place, so the smoother starts from the blended first
    // point, which is point 314. Starting from the raw first point left a notch at the seam.
    float radius = baseRadius + sampleAt(kCirclePoints) * amplitude;
    for (int i = 0; i < kCirclePoints; ++i) {
        float sample = sampleAt(i);
        if (i < kCircleBlend) {
            const float blend = static_cast<float>(i) / static_cast<float>(kCircleBlend);
            sample = sample * blend + sampleAt(i + kCirclePoints) * (1.f - blend);
        }
        radius = radius * 0.5f + 0.5f * (baseRadius + sample * amplitude);
        if (radius < 5.f * toPixels) {
            continue;
        }
        const float angle = static_cast<float>(i) * 0.02f;
        plot(indices, width, height, static_cast<int>(centerX + radius * std::cos(angle)),
             static_cast<int>(centerY + radius * std::sin(angle)), ink);
    }
}

void drawShape(std::uint8_t* indices, int width, int height, const AudioFrame& audio, Ink ink,
               int waveform, int mode, float centerXNorm, float centerYNorm, float motionTime) {
    switch (waveform) {
        case 2:
            drawTwoLines(indices, width, height, audio, ink, centerYNorm);
            break;
        case 3:
            drawVerticalLine(indices, width, height, audio, ink, centerXNorm);
            break;
        case 4:
            drawDiagonals(indices, width, height, audio, ink);
            break;
        case 5:
            drawCircle(indices, width, height, audio, ink, centerXNorm, centerYNorm);
            break;
        case 6:
            drawScope(indices, width, height, audio, ink, centerXNorm, centerYNorm, motionTime);
            break;
        default:
            drawLine(indices, width, height, audio, ink, mode);
            break;
    }
}

void drawWaveform(std::uint8_t* indices, int width, int height, const AudioFrame& audio,
                  int waveform, int mode, float centerXNorm, float centerYNorm, float motionTime) {
    if (indices == nullptr || audio.silent || audio.samples.empty() || width < 2 || height < 2) {
        return;
    }
    // Beat-mode brightness follows each hit and goes dark between them. As in the
    // original, the X-Y oscilloscope keeps its steady brightness.
    const float beatScale = audio.beatMode && waveform != 6 ? std::clamp(audio.beatBrightness, 0.f, 1.f) : 1.f;
    const int bright = static_cast<int>(waveBrightness(audio.volume, audio.averageVolume) * beatScale);
    if (bright < 1) {
        return;
    }
    drawShape(indices, width, height, audio, Ink{bright, false}, waveform, mode, centerXNorm, centerYNorm, motionTime);
}

// The idle wave's drift phases at one moment. See `wrappedPhase`.
struct IdlePhases {
    float left = 0;
    float leftPartial = 0;
    float right = 0;
    float rightPartial = 0;
};

IdlePhases idlePhases(double seconds) {
    return {wrappedPhase(0.31, seconds), wrappedPhase(-0.17, seconds), wrappedPhase(0.23, seconds),
            wrappedPhase(0.13, seconds)};
}

// The idle wave's signal at column position u (0...1). Left runs three slow cycles across
// the frame and right two, so the oscilloscope draws a 3:2 figure. Each side adds a
// faster, smaller partial drifting the other way, so the shape keeps changing without
// repeating soon.
float idleLeft(float u, const IdlePhases& phases) {
    constexpr auto kTurn = static_cast<float>(kTau);
    return 0.65f * std::sin(kTurn * 3.f * u + phases.left) +
           0.35f * std::sin(kTurn * 5.f * u + phases.leftPartial + 1.3f);
}

float idleRight(float u, const IdlePhases& phases) {
    constexpr auto kTurn = static_cast<float>(kTau);
    return 0.65f * std::sin(kTurn * 2.f * u + phases.right + 0.7f) +
           0.35f * std::sin(kTurn * 7.f * u + phases.rightPartial);
}

}  // namespace

const char* waveformName(int waveform) {
    static constexpr std::array<const char*, kLastWaveform> kNames = {
        "Line", "Two lines", "Vertical line", "Diagonals", "Circle", "Oscilloscope",
    };
    if (waveform < kFirstWaveform || waveform > kLastWaveform) {
        return "?";
    }
    return kNames.at(static_cast<std::size_t>(waveform - kFirstWaveform));
}

bool lookAllowed(int mode, int waveform) {
    if (mode < kFirstMapMode || mode > kLastMapMode || waveform < kFirstWaveform || waveform > kLastWaveform) {
        return false;
    }
    // The pairs GenerateChunkOfNewMap re-rolls.
    return !((mode == 6 && waveform == 5) ||
             (mode == 12 && (waveform == 4 || waveform == 6)) ||
             (mode == 14 && (waveform == 3 || waveform == 4)) ||
             ((mode == 8 || mode == 23 || mode == 24) && waveform == 6));
}

Look randomLook(Random& random) {
    // FX_Pick_Random_Mode: uniform, then a 1-in-25 chance each of fuzzy (7) and super-perspective (5).
    Look look;
    look.mode = kFirstMapMode + random.below(kLastMapMode);
    if (random.below(25) == 0) {
        look.mode = 7;
    }
    if (random.below(25) == 0) {
        look.mode = 5;
    }
    // (rand() % (NUM_WAVES*3 - 1)) / 3 + 1: waveforms 1...5 at 3/17 each and 6 at 2/17.
    do {
        look.waveform = random.below(kLastWaveform * 3 - 1) / 3 + 1;
    } while (!lookAllowed(look.mode, look.waveform));
    return look;
}

// The effect layer draws its particles from its own generator, so the frame rate does not
// change which looks come up.
// The slide shift draws its offsets from its own generator for the same reason.
Engine::Engine(std::uint32_t seed) : effectLayer_(seed ^ 0x5BD1E995u), slideShift_(seed ^ 0x27D4EB2Fu) {
    random_.state = seed == 0 ? 1u : seed;
    pickEffects();
    pickPalette(false);
    slideShift_.startLook(SlideShift::rollAllowed(random_));
    resize(960, 540);
}

void Engine::resize(int width, int height) {
    width = std::max(width, 2);
    height = std::max(height, 2);
    if (width == width_ && height == height_) {
        return;
    }
    const int oldWidth = width_;
    const int oldHeight = height_;
    width_ = width;
    height_ = height;
    const std::size_t count = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);

    // Carry the picture into the new size, nearest-neighbor, so a live resize does not go black.
    back_.assign(count, 0);
    if (oldWidth >= 2 && oldHeight >= 2 && !front_.empty()) {
        for (int y = 0; y < height_; ++y) {
            const int sy = y * oldHeight / height_;
            for (int x = 0; x < width_; ++x) {
                const int sx = x * oldWidth / width_;
                back_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)] =
                    front_[static_cast<std::size_t>(sy) * static_cast<std::size_t>(oldWidth) + static_cast<std::size_t>(sx)];
            }
        }
    }
    front_.swap(back_);
    back_.assign(count, 0);
    rgba_.assign(count * 4, 0);
    expandPalette(front_.data());
    effectLayer_.clearTrail();
    rebuildMap();
}

void Engine::setPaused(bool paused) { paused_ = paused; }

void Engine::setLocked(bool locked) { locked_ = locked; }

void Engine::pickLook() {
    if (width_ < 2 || height_ < 2) {
        return;
    }
    // Next and the timer always change the look, so the current pair is drawn again.
    Look picked = randomLook(random_);
    while (picked.mode == mapParameters_.mode && picked.waveform == waveform_) {
        picked = randomLook(random_);
    }
    mapParameters_ = randomMapParameters(picked.mode, width_, height_, random_);
    waveform_ = picked.waveform;
    pickEffects();
    pickPalette(true);
    slideShift_.startLook(SlideShift::rollAllowed(random_));
}

// FX_Random_Palette ran with each new map. Like the effects, it reads whether there was sound.
void Engine::pickPalette(bool blend) {
    paletteRecipe_ = randomPaletteRecipe(random_);
    fromPalette_ = palette_;
    targetPalette_ = buildPalette(paletteRecipe_, soundEmpty());
    paletteBlendLeft_ = blend ? kPaletteBlendSeconds : 0;
    if (!blend) {
        palette_ = targetPalette_;
    }
}

bool Engine::soundEmpty() const { return silentSeconds_ > kSoundEmptySeconds; }

void Engine::setPalette(const PaletteRecipe& recipe) {
    paletteRecipe_ = clampedPaletteRecipe(recipe);
    targetPalette_ = buildPalette(paletteRecipe_, soundEmpty());
    palette_ = targetPalette_;
    paletteBlendLeft_ = 0;
    expandPalette(front_.data());
}

void Engine::pickEffects() {
    effects_ = randomEffects(mapParameters_.mode, soundActive_, random_);
    mapParameters_.nuclide = effects_.has(Effect::Nuclide);
    effectLayer_.startLook(effects_);
}

void Engine::setEffects(const EffectSet& effects) {
    effects_ = effects;
    effectLayer_.startLook(effects_);
    const bool nuclide = effects_.has(Effect::Nuclide);
    if (nuclide != mapParameters_.nuclide) {
        mapParameters_.nuclide = nuclide;
        if (mapParameters_.mode == 5) {
            rebuildMap();
        }
    }
}

void Engine::nextVisual() {
    pickLook();
    mapElapsed_ = 0;
    rebuildMap();
}

void Engine::rebuildMap() {
    buildMap(map_, width_, height_, mapParameters_, bands_);
}

void Engine::expandPalette(const std::uint8_t* indices) {
    const std::size_t count = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    // From the palette as drawn, so the levels follow a blend. The palette only changes while
    // it blends or on a new pick, so the table is rebuilt then and reused otherwise.
    if (!lookupBuilt_ || palette_ != levelsPalette_) {
        levelsPalette_ = palette_;
        lookupBuilt_ = true;
        const HighlightLevels levels = highlightLevels(palette_);
        for (std::size_t n = 0; n < rgbaLookup_.size(); ++n) {
            rgbaLookup_[n] = {palette_[n * 3], palette_[n * 3 + 1], palette_[n * 3 + 2], levels[n]};
        }
    }
    // One four-byte copy per pixel, which compiles to a single store.
    std::uint8_t* out = rgba_.data();
    for (std::size_t i = 0; i < count; ++i) {
        std::memcpy(out + i * 4, rgbaLookup_[indices[i]].data(), 4);
    }
    frameChanged_ = true;
}

FrameView Engine::tick(double dt, const AudioFrame& audio) {
    FrameView view;
    view.rgba = rgba_.data();
    view.width = width_;
    view.height = height_;
    if (width_ < 2 || height_ < 2 || paused_) {
        // A resize or a new palette while paused still changes the pixels once.
        view.changed = frameChanged_;
        frameChanged_ = false;
        return view;
    }
    if (dt < 0) {
        dt = 0;
    }
    if (dt > kMaxDt) {
        dt = kMaxDt;
    }

    soundActive_ = !audio.silent && !audio.samples.empty();
    silentSeconds_ = soundActive_ ? 0 : silentSeconds_ + dt;
    // Sound whose line would draw dimmer than the idle wave counts as quiet for it. The beat
    // scale is left out, so the dark gaps between hits in beat mode do not bring it up.
    const bool faint = !soundActive_ || waveBrightness(audio.volume, audio.averageVolume) < kIdleHandOff;
    faintSeconds_ = faint ? faintSeconds_ + dt : 0;
    if (!locked_) {
        mapElapsed_ += dt;
        if (mapElapsed_ >= kMapInterval && lookMayChange(mapElapsed_ - kMapInterval, audio)) {
            pickLook();
            mapElapsed_ = 0;
            rebuildMap();
        }
    }

    // The blend finishes even while locked, since lock only stops new picks.
    if (paletteBlendLeft_ > 0) {
        paletteBlendLeft_ = std::max(paletteBlendLeft_ - dt, 0.0);
        palette_ = blendPalettes(fromPalette_, targetPalette_,
                                 static_cast<float>(1.0 - paletteBlendLeft_ / kPaletteBlendSeconds));
    }

    motionTime_ += dt;
    // Dampened modes move half as far per frame, as mode_motion_dampened did.
    const float frameStep = static_cast<float>(std::min(dt * kReferenceFps, 1.0));
    const float step = frameStep * mapParameters_.damping;
    // The shift keeps following peaks while locked, like the rest of the audio motion.
    slideShift_.update(dt, audio, width_);
    const float perReferenceFrame =
        static_cast<float>(weightNumerator(width_, height_)) / 256.f * mapParameters_.decay;
    const float decay = static_cast<float>(std::pow(static_cast<double>(perReferenceFrame), dt * kReferenceFps));
    // The effects draw into the previous frame before the warp, as the original drew into VS1.
    effectLayer_.draw(front_.data(), width_, height_, effects_, mapParameters_.mode,
                      mapParameters_.centerXNorm * static_cast<float>(width_),
                      mapParameters_.centerYNorm * static_cast<float>(height_), dt * kReferenceFps, audio);
    // The original added slider1 to the source pointer once per frame, whatever the map's damping.
    warpIndices(front_.data(), back_.data(), width_, height_, map_.data(), step, decay,
                slideShift_.x() * frameStep, slideShift_.y() * frameStep, ++ditherSeed_, bands_);
    if (!effects_.hideWaveform) {
        drawWaveform(back_.data(), width_, height_, audio, waveform_, mapParameters_.mode, mapParameters_.centerXNorm,
                     mapParameters_.centerYNorm, static_cast<float>(motionTime_));
    }
    updateIdle(dt);
    drawIdleWave();
    expandPalette(back_.data());
    front_.swap(back_);
    frameChanged_ = false;
    return view;
}

void Engine::updateIdle(double dt) {
    if (!idleWaveEnabled_) {
        idleLevel_ = 0;
        return;
    }
    if (faintSeconds_ <= 0) {
        idleLevel_ = std::max(idleLevel_ - dt / kIdleFadeSeconds, 0.0);
        return;
    }
    // faintSeconds_ already includes this tick. Only the part of it past the wait counts, so
    // the rise keeps the same pace at any frame rate. An idle wave still fading out skips it.
    const double pastWait = idleLevel_ > 0 ? dt : std::min(dt, faintSeconds_ - kIdleWaitSeconds);
    if (pastWait > 0) {
        idleLevel_ = std::min(idleLevel_ + pastWait / kIdleRiseSeconds, 1.0);
    }
}

// Drawn after the warp, like the waveform. A look whose nuclide replaces the waveform draws
// the idle wave as the circle, a stand-in for the nuclide's resting ring.
void Engine::drawIdleWave() {
    if (idleLevel_ <= 0) {
        return;
    }
    const auto level = static_cast<float>(idleLevel_);
    const int bright = static_cast<int>(
        level * (kIdleBright + kIdleBreath * static_cast<float>(std::sin(motionTime_ * kTau / kIdleBreathSeconds))));
    if (bright < 1) {
        return;
    }
    const float amplitude =
        level * (kIdleAmplitude + kIdleSwell * static_cast<float>(std::sin(motionTime_ * kTau / kIdleSwellSeconds)));
    const IdlePhases phases = idlePhases(motionTime_);
    const auto columns = static_cast<std::size_t>(width_);
    idleFrame_.samples.resize(columns);
    idleFrame_.left.resize(columns);
    idleFrame_.right.resize(columns);
    for (std::size_t x = 0; x < columns; ++x) {
        const float u = static_cast<float>(x) / static_cast<float>(columns - 1);
        const float left = idleLeft(u, phases) * amplitude;
        const float right = idleRight(u, phases) * amplitude;
        idleFrame_.left[x] = left;
        idleFrame_.right[x] = right;
        idleFrame_.samples[x] = 0.5f * (left + right);
    }
    const int waveform = effects_.hideWaveform ? 5 : waveform_;
    // Soft spots: the idle wave is slow and faint, and sharp dots looked out of place on it.
    drawShape(back_.data(), width_, height_, idleFrame_, Ink{bright, true}, waveform, mapParameters_.mode,
              mapParameters_.centerXNorm, mapParameters_.centerYNorm, static_cast<float>(motionTime_));
}

}  // namespace geiss
