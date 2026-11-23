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
// A new look's palette fades in over this many seconds of motion. The window opens on
// kFirstPalette, as it opens on mode 25 and waveform 1.
constexpr double kPaletteFadeSeconds = 2.0;
constexpr int kFirstPalette = 0;
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
// oscilloscope traces a 3:2 Lissajous figure that slowly turns and morphs. It is drawn in
// the drift color at a low brightness, so it is calm and never glows on HDR.
constexpr double kIdleWaitSeconds = 1.5;
constexpr double kIdleRiseSeconds = 4.0;
// On RenderWave's 0...155 brightness scale: the wave breathes between about 26 and 46 over
// kIdleBreathSeconds. The drift's brightest channel peaks near 2.5 times that, about 114,
// a dim part of the palette, so it never glows.
constexpr float kIdleBright = 36.f;
constexpr float kIdleBreath = 10.f;
// The hand-off. Sound whose line would draw dimmer than the idle wave's darkest keeps the
// idle wave, so quiet music never looks darker than silence; its faint line plays inside it.
// kIdleHandOff is the idle wave's darkest base. The two are compared as drawn colors (see
// `drawsDimmerThanIdle`): at the same base the drift color is brighter than a line's.
// Brighter sound fades the idle wave out over kIdleFadeSeconds instead of cutting it, and
// while any of it is left, quiet brings it back up without the wait, so sound hovering
// near the hand-off makes it dip instead of blinking off and on.
constexpr float kIdleHandOff = kIdleBright - kIdleBreath;
// The drift color's brightest channel averages 1.58 times its base over time, for any drift
// (measured over many drifts; each channel swings 0.36...2.49 times). The hand-off compares
// against that average, so a steady line does not flip across it as the drift wanders.
constexpr float kDriftMeanPeak = 1.58f;
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
// 0...155. The wave color tints it per channel, up to 255.
constexpr float kBaseCeiling = 155.f;

float waveBrightness(float volume, float averageVolume) {
    return std::clamp(volume * 4.f + averageVolume * 0.4f - 10.f, 0.f, kBaseCeiling);
}

// How a waveform's points are drawn: the color, and whether each point is a soft spot
// instead of one sharp pixel.
struct Ink {
    Rgb color;
    bool soft = false;
};

void raise(std::uint8_t& channel, int value) {
    if (channel < value) {
        channel = static_cast<std::uint8_t>(std::min(value, 255));
    }
}

// Max-blends each channel, as every original waveform did in truecolor. A pixel whose
// brightness the line now sets is the line's, so it is no longer held to SDR. A line that only
// lifts a dimmer channel leaves the held level, so a texture's bright channel keeps from
// glowing.
void plotPixel(std::uint8_t* pixels, int width, int height, int x, int y, Rgb color) {
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }
    std::uint8_t* p = pixels + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * kPixelBytes;
    const int before = std::max({p[0], p[1], p[2]});
    raise(p[0], color.r);
    raise(p[1], color.g);
    raise(p[2], color.b);
    if (std::max({color.r, color.g, color.b}) > before) {
        p[kHeldByte] = 0;
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

void plotSoftSpot(std::uint8_t* pixels, int width, int height, int x, int y, Rgb color) {
    if (pixels == nullptr) {
        return;
    }
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int weight = kSoftSpot.at((static_cast<std::size_t>(dy + 2) * 5) + static_cast<std::size_t>(dx + 2));
            plotPixel(pixels, width, height, x + dx, y + dy,
                      {color.r * weight / 256, color.g * weight / 256, color.b * weight / 256});
        }
    }
}

float wrappedPhase(double radiansPerSecond, double seconds) {
    return static_cast<float>(std::fmod(radiansPerSecond * seconds, kTau));
}

bool drawsDimmerThanIdle(float base, double frames, const ColorDrift& drift,
                         const std::array<float, kFourierDetail>& power) {
    const auto brightest = [](Rgb color) { return std::max({color.r, color.g, color.b}); };
    // Without spectrum power the line takes the drift color too, so the two compare by base.
    if (std::all_of(power.begin(), power.end(), [](float band) { return band <= 0.f; })) {
        return base < kIdleHandOff;
    }
    return static_cast<float>(brightest(waveColor(base, frames, drift, power))) < kIdleHandOff * kDriftMeanPeak;
}

namespace {

void plot(std::uint8_t* pixels, int width, int height, int x, int y, Ink ink) {
    if (ink.soft) {
        plotSoftSpot(pixels, width, height, x, y, ink.color);
    } else {
        plotPixel(pixels, width, height, x, y, ink.color);
    }
}

// RenderWave waveform 1: one smoothed line, drawn with a max blend.
// It reads the mono columns and stays on the frame center. Over mode 10's scroll the
// original lowered it to three quarters of the height.
void drawLine(std::uint8_t* pixels, int width, int height, const AudioFrame& audio, Ink ink, int mode) {
    const float amplitude = static_cast<float>(height) * kWaveFraction;
    const float yCenter = static_cast<float>(height) * (mode == 10 ? 0.75f : 0.5f);
    float previous = columnSample(audio.samples, 0, width) * amplitude + yCenter;
    for (int x = 0; x < width; ++x) {
        const float z = columnSample(audio.samples, x, width) * amplitude + yCenter;
        previous = previous * 0.9f + z * 0.1f;
        plot(pixels, width, height, x, static_cast<int>(previous), ink);
    }
}

// RenderWave waveform 2: two smoothed lines. Left sits 12% of the height above the
// map center, right sits 12% below, and both use 0.7 of the line amplitude.
// Intentional difference: each column uses that column's channel. The original
// interleaved buffer repeated each stereo pair across two pixels.
void drawTwoLines(std::uint8_t* pixels, int width, int height, const AudioFrame& audio,
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
        plot(pixels, width, height, x, static_cast<int>(previousLeft), ink);
        plot(pixels, width, height, x, static_cast<int>(previousRight), ink);
    }
}

// RenderWave waveform 6: an X-Y oscilloscope. 314 smoothed stereo points, scaled by 1.2,
// rocked around the map center. The original seeds the smoother with the unscaled first pair.
// Intentional difference: the original walks the first 314 frames of the capture buffer.
// This frame is already one sample per column, so the 314 points cover that whole wave.
void drawScope(std::uint8_t* pixels, int width, int height, const AudioFrame& audio,
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
        plot(pixels, width, height, static_cast<int>(px + centerX), static_cast<int>(py + centerY), ink);
    }
}

// RenderWave waveform 3: one smoothed vertical line through the map center. Row y reads
// the sample of column y, as the original read its buffer by row.
void drawVerticalLine(std::uint8_t* pixels, int width, int height, const AudioFrame& audio,
                      Ink ink, float centerXNorm) {
    const float amplitude = static_cast<float>(height) * kWaveFraction;
    const float xCenter = centerXNorm * static_cast<float>(width);
    float previous = columnSample(audio.samples, 0, width) * amplitude + xCenter;
    for (int y = 0; y < height; ++y) {
        previous = previous * 0.9f + (columnSample(audio.samples, y, width) * amplitude + xCenter) * 0.1f;
        plot(pixels, width, height, static_cast<int>(previous), y, ink);
    }
}

// RenderWave waveform 4: two smoothed diagonal lines at 0.9 of the line amplitude. Left
// runs down from the top-left corner and right from width - height, both one pixel
// across per row.
void drawDiagonals(std::uint8_t* pixels, int width, int height, const AudioFrame& audio, Ink ink) {
    const float amplitude = static_cast<float>(height) * kWaveFraction * 0.9f;
    const std::vector<float>& left = audio.left.empty() ? audio.samples : audio.left;
    const std::vector<float>& right = audio.right.empty() ? left : audio.right;
    float previousLeft = columnSample(left, 0, width) * amplitude;
    float previousRight = columnSample(right, 0, width) * amplitude;
    for (int y = 0; y < height; ++y) {
        previousLeft = previousLeft * 0.9f + columnSample(left, y, width) * amplitude * 0.1f;
        previousRight = previousRight * 0.9f + columnSample(right, y, width) * amplitude * 0.1f;
        plot(pixels, width, height, static_cast<int>(previousLeft) + y, y, ink);
        plot(pixels, width, height, static_cast<int>(previousRight) + y + (width - height), y, ink);
    }
}

// RenderWave waveform 5: a circle around the map center whose radius follows the mono
// wave at 0.7 of the line amplitude. The base radius is 60 pixels on a 640-wide frame,
// and points closer than 5 (at 640) to the center are skipped.
// Intentional difference: like waveform 6, the 364 points cover the whole column wave
// instead of the first 364 frames of the capture buffer.
void drawCircle(std::uint8_t* pixels, int width, int height, const AudioFrame& audio,
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
        plot(pixels, width, height, static_cast<int>(centerX + radius * std::cos(angle)),
             static_cast<int>(centerY + radius * std::sin(angle)), ink);
    }
}

void drawShape(std::uint8_t* pixels, int width, int height, const AudioFrame& audio, Ink ink,
               int waveform, int mode, float centerXNorm, float centerYNorm, float motionTime) {
    switch (waveform) {
        case 2:
            drawTwoLines(pixels, width, height, audio, ink, centerYNorm);
            break;
        case 3:
            drawVerticalLine(pixels, width, height, audio, ink, centerXNorm);
            break;
        case 4:
            drawDiagonals(pixels, width, height, audio, ink);
            break;
        case 5:
            drawCircle(pixels, width, height, audio, ink, centerXNorm, centerYNorm);
            break;
        case 6:
            drawScope(pixels, width, height, audio, ink, centerXNorm, centerYNorm, motionTime);
            break;
        default:
            drawLine(pixels, width, height, audio, ink, mode);
            break;
    }
}

// `frames` is the color clock in reference frames.
void drawWaveform(std::uint8_t* pixels, int width, int height, const AudioFrame& audio, int waveform, int mode,
                  float centerXNorm, float centerYNorm, float motionTime, double frames, const ColorDrift& drift) {
    if (pixels == nullptr || audio.silent || audio.samples.empty() || width < 2 || height < 2) {
        return;
    }
    // Beat-mode brightness follows each hit and goes dark between them. As in the
    // original, the X-Y oscilloscope keeps its steady brightness.
    const float beatScale = audio.beatMode && waveform != 6 ? std::clamp(audio.beatBrightness, 0.f, 1.f) : 1.f;
    const float base = waveBrightness(audio.volume, audio.averageVolume) * beatScale;
    const Rgb color = waveColor(base, frames, drift, audio.power);
    if (color.r < 1 && color.g < 1 && color.b < 1) {
        return;
    }
    drawShape(pixels, width, height, audio, Ink{color, false}, waveform, mode, centerXNorm, centerYNorm, motionTime);
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
    if (!isMapMode(mode) || waveform < kFirstWaveform || waveform > kLastWaveform) {
        return false;
    }
    // The pairs GenerateChunkOfNewMap re-rolls.
    return !((mode == 6 && waveform == 5) ||
             (mode == 14 && (waveform == 3 || waveform == 4)) ||
             ((mode == 8 || mode == 23 || mode == 24) && waveform == 6));
}

Look randomLook(Random& random) {
    // FX_Pick_Random_Mode: uniform, then a 1-in-25 chance each of fuzzy (7) and super-perspective (5).
    Look look;
    // Mode 12 is left out: one draw over the other 24 modes, skipping 12, so the mode takes a
    // single draw as in the original.
    look.mode = kFirstMapMode + random.below(kLastMapMode - 1);
    if (look.mode >= kRetiredMapMode) {
        ++look.mode;
    }
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
// The color drift comes from a third generator, so adding it left the looks unchanged.
Engine::Engine(std::uint32_t seed) : effectLayer_(seed ^ 0x5BD1E995u), slideShift_(seed ^ 0x27D4EB2Fu) {
    random_.state = seed == 0 ? 1u : seed;
    Random driftRandom;
    driftRandom.state = (seed ^ 0x68E31DA4u) == 0 ? 1u : seed ^ 0x68E31DA4u;
    drift_ = randomColorDrift(driftRandom);
    effectLayer_.setColorDrift(drift_);
    paletteRandom_.state = (seed ^ 0x165667B1u) == 0 ? 1u : seed ^ 0x165667B1u;
    paletteFrom_ = kFirstPalette;
    paletteTo_ = kFirstPalette;
    pickEffects();
    slideShift_.startLook(SlideShift::rollAllowed(random_));
    resize(960, 540);
}

void Engine::resize(int width, int height) {
    width = std::max(width, 2);
    height = std::max(height, 2);
    if (width == width_ && height == height_) {
        return;
    }
    // The buffers are about to change, so a running warp must finish, and its frame is lost.
    finishWarp();
    warpReady_ = false;
    lastDt_ = 0;
    const int oldWidth = width_;
    const int oldHeight = height_;
    width_ = width;
    height_ = height;
    const std::size_t count = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);

    // Carry the picture into the new size, nearest-neighbor, so a live resize does not go black.
    back_.assign(count * kPixelBytes, 0);
    if (oldWidth >= 2 && oldHeight >= 2 && !front_.empty()) {
        for (int y = 0; y < height_; ++y) {
            const int sy = y * oldHeight / height_;
            for (int x = 0; x < width_; ++x) {
                const int sx = x * oldWidth / width_;
                std::memcpy(&back_[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)) * kPixelBytes],
                            &front_[(static_cast<std::size_t>(sy) * static_cast<std::size_t>(oldWidth) + static_cast<std::size_t>(sx)) * kPixelBytes],
                            kPixelBytes);
            }
        }
    }
    front_.swap(back_);
    back_.assign(count * kPixelBytes, 0);
    map_.assign(count, SourcePoint{});
    rgba_.assign(hostExpands_ ? 0 : count * 4, 0);
    ++generation_;
    expandPixels(front_.data());
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
    slideShift_.startLook(SlideShift::rollAllowed(random_));
    pickPalette();
}

void Engine::pickPalette() {
    // A look that comes during a fade starts from whichever palette is nearer on screen.
    paletteFrom_ = paletteBlend_ < 0.5 ? paletteFrom_ : paletteTo_;
    int picked = paletteRandom_.below(paletteCount() - 1);
    if (picked >= paletteFrom_) {
        ++picked;
    }
    paletteTo_ = picked;
    paletteBlend_ = 0;
}

const PaletteLut& Engine::paletteLut(float headroom) {
    if (paletteFrom_ != lutFrom_ || paletteTo_ != lutTo_ || paletteBlend_ != lutBlend_ || headroom != lutHeadroom_) {
        // Ease the fade in and out.
        const double t = paletteBlend_;
        const auto eased = static_cast<float>(t * t * (3 - 2 * t));
        buildPaletteLut(savedPalette(paletteFrom_), savedPalette(paletteTo_), eased, headroom, lut_);
        lutFrom_ = paletteFrom_;
        lutTo_ = paletteTo_;
        lutBlend_ = paletteBlend_;
        lutHeadroom_ = headroom;
    }
    return lut_;
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
    // A started warp keeps the map it began with.
    finishWarp();
    if (map_.size() == static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_)) {
        buildMap(map_.data(), width_, height_, mapParameters_, bands_);
    }
}

std::array<std::uint8_t, 4> displayPixel(const std::uint8_t* pixel) {
    const int brightest = std::max({pixel[0], pixel[1], pixel[2]});
    const int held = std::min(static_cast<int>(pixel[kHeldByte]), brightest);
    const int share = brightest > 0 ? held * 255 / brightest : 0;
    return {pixel[0], pixel[1], pixel[2], static_cast<std::uint8_t>(share)};
}

void Engine::setHostExpands(bool hostExpands) {
    if (hostExpands == hostExpands_) {
        return;
    }
    hostExpands_ = hostExpands;
    const std::size_t count = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    rgba_.assign(hostExpands_ ? 0 : count * 4, 0);
    expandPixels(front_.data());
}

EngineBuffers Engine::buffers() {
    EngineBuffers buffers;
    buffers.pixelsA = front_.data();
    buffers.pixelsB = back_.data();
    buffers.pixelBytes = pageRounded(front_.size());
    buffers.map = map_.data();
    buffers.mapBytes = pageRounded(map_.size() * sizeof(SourcePoint));
    buffers.generation = generation_;
    return buffers;
}

void Engine::expandPixels(const std::uint8_t* pixels) {
    frameChanged_ = true;
    if (hostExpands_) {
        return;
    }
    const std::size_t count = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    std::uint8_t* out = rgba_.data();
    for (std::size_t i = 0; i < count; ++i) {
        const std::array<std::uint8_t, 4> rgba = displayPixel(pixels + i * kPixelBytes);
        std::memcpy(out + i * 4, rgba.data(), 4);
    }
}

FrameView Engine::tick(double dt, const AudioFrame& audio) {
    // A warp started after the last tick has had the time between frames to run.
    finishWarp();
    FrameView view;
    view.rgba = hostExpands_ ? nullptr : rgba_.data();
    view.pixels = front_.data();
    view.width = width_;
    view.height = height_;
    if (width_ < 2 || height_ < 2 || paused_) {
        // A resize while paused still changes the pixels once.
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
    // Sound whose line would draw dimmer than the idle wave counts as quiet for it. The beat
    // scale is left out, so the dark gaps between hits in beat mode do not bring it up.
    const bool faint = !soundActive_ || drawsDimmerThanIdle(waveBrightness(audio.volume, audio.averageVolume),
                                                            motionTime_ * kReferenceFps, drift_, audio.power);
    faintSeconds_ = faint ? faintSeconds_ + dt : 0;
    if (!locked_) {
        mapElapsed_ += dt;
        if (mapElapsed_ >= kMapInterval && lookMayChange(mapElapsed_ - kMapInterval, audio)) {
            pickLook();
            mapElapsed_ = 0;
            rebuildMap();
        }
    }

    motionTime_ += dt;
    paletteBlend_ = std::min(1.0, paletteBlend_ + dt / kPaletteFadeSeconds);
    // The shift keeps following peaks while locked, like the rest of the audio motion.
    slideShift_.update(dt, audio, width_);
    if (!warpReady_) {
        drawEffectsAndWarp(dt, audio);
    }
    finishWarp();
    warpReady_ = false;
    if (!effects_.hideWaveform) {
        drawWaveform(back_.data(), width_, height_, audio, waveform_, mapParameters_.mode, mapParameters_.centerXNorm,
                     mapParameters_.centerYNorm, static_cast<float>(motionTime_), motionTime_ * kReferenceFps, drift_);
    }
    updateIdle(dt);
    drawIdleWave();
    expandPixels(back_.data());
    front_.swap(back_);
    frameChanged_ = false;
    lastDt_ = dt;
    view.pixels = front_.data();
    return view;
}

void Engine::drawEffectsAndWarp(double dt, const AudioFrame& audio) {
    // Dampened modes move half as far per frame, as mode_motion_dampened did.
    const float frameStep = static_cast<float>(std::min(dt * kReferenceFps, 1.0));
    const float step = frameStep * mapParameters_.damping;
    const float perReferenceFrame =
        static_cast<float>(weightNumerator(width_, height_)) / 256.f;
    const float decay = static_cast<float>(std::pow(static_cast<double>(perReferenceFrame), dt * kReferenceFps));
    // The effects draw into the previous frame before the warp, as the original drew into VS1.
    effectLayer_.draw(front_.data(), width_, height_, effects_, mapParameters_.mode,
                      mapParameters_.centerXNorm * static_cast<float>(width_),
                      mapParameters_.centerYNorm * static_cast<float>(height_), dt * kReferenceFps, audio);
    // The original added slider1 to the source pointer once per frame, whatever the map's damping.
    // Clamped here as warpPixels clamps them, so a host warp sees the same values.
    const WarpJob warp{front_.data(),
                       back_.data(),
                       width_,
                       height_,
                       map_.data(),
                       std::clamp(step, 0.f, 1.f),
                       std::clamp(decay, 0.f, 1.f),
                       slideShift_.x() * frameStep,
                       slideShift_.y() * frameStep,
                       ++ditherSeed_};
    if (warp_.run != nullptr) {
        warp_.run(warp_.host, warp);
        warpPending_ = warp_.wait != nullptr;
    } else {
        warpPixels(warp.src, warp.dst, warp.width, warp.height, warp.map, warp.step, warp.decay, warp.shiftX,
                   warp.shiftY, warp.ditherSeed, bands_);
    }
    warpReady_ = true;
}

void Engine::startNextWarp(const AudioFrame& audio) {
    if (warp_.run == nullptr || warp_.wait == nullptr || paused_ || warpReady_ || lastDt_ <= 0 || width_ < 2 ||
        height_ < 2) {
        return;
    }
    drawEffectsAndWarp(lastDt_, audio);
}

void Engine::finishWarp() {
    if (warpPending_) {
        warpPending_ = false;
        warp_.wait(warp_.host);
    }
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
    const float base =
        level * (kIdleBright + kIdleBreath * static_cast<float>(std::sin(motionTime_ * kTau / kIdleBreathSeconds)));
    const Rgb color = driftColor(base, motionTime_ * kReferenceFps, drift_);
    if (color.r < 1 && color.g < 1 && color.b < 1) {
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
    drawShape(back_.data(), width_, height_, idleFrame_, Ink{color, true}, waveform, mapParameters_.mode,
              mapParameters_.centerXNorm, mapParameters_.centerYNorm, static_cast<float>(motionTime_));
}

}  // namespace geiss
