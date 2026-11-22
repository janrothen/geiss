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
#include "geiss/bands.hpp"
#include "geiss/color.hpp"
#include "geiss/effects.hpp"
#include "geiss/map.hpp"
#include "geiss/shift.hpp"

#include <array>
#include <cstdint>

namespace geiss {

constexpr int kFirstWaveform = 1;
constexpr int kLastWaveform = 6;

// A short name for the waveform, from the spec's list, or "?" for an unknown number.
const char* waveformName(int waveform);

// One map mode and one waveform.
struct Look {
    int mode = 25;
    int waveform = 1;
};

// Max-blends a soft 5x5 spot centered on (x, y) into a pixel buffer (kPixelBytes per pixel),
// per channel: `color` at the center, about two thirds of it beside, falling to a twentieth at
// the corners. Clipped at the edges.
// The idle wave draws its points this way, so points up to about three pixels apart, such as
// the circle's at a large radius, run together into a soft line instead of a row of dots.
void plotSoftSpot(std::uint8_t* pixels, int width, int height, int x, int y, Rgb color);

// radiansPerSecond * seconds, worked out in double and wrapped to within one turn of zero
// (keeping its sign) before narrowing to float. The idle wave's drift uses it, so its motion
// stays smooth however long the engine has run.
float wrappedPhase(double radiansPerSecond, double seconds);

// The idle wave's hand-off: whether a line of brightness `base` (0...155, RenderWave's scale,
// before the beat scale) in the wave color would draw dimmer than the idle wave at its darkest,
// the line at `frames` reference frames. It compares the colors as drawn, by their brightest
// channel, since the drift color the idle wave uses sits further above its base than the
// spectrum color of a sound's line. The idle side is the drift's average over time, about 41,
// so the threshold holds still while the drift wanders.
bool drawsDimmerThanIdle(float base, double frames, const ColorDrift& drift,
                         const std::array<float, kFourierDetail>& power);

// False for the map and waveform pairs the original re-rolls, and for unknown numbers.
bool lookAllowed(int mode, int waveform);

// The original's pick: a uniform mode with extra chances for 7 and 5, and a waveform
// with 6 less likely, re-rolled until the pair is allowed.
Look randomLook(Random& random);

// RGB is the simulated color, doubled with saturation as the original's truecolor blit showed
// it. Alpha carries the pixel's highlight level, rising from 0 to 255 as its brightest stored
// channel goes from 128 to 208, so a presenter can pick out the hottest cores. It is scaled by
// the share of that channel not held to SDR (see kHeldByte), so fixed textures never glow.
// The picture itself is opaque.
struct FrameView {
    const std::uint8_t* rgba = nullptr;
    int width = 0;
    int height = 0;
    // False when the pixels are the same as the previous view's, for example while paused,
    // so a presenter can skip the upload.
    bool changed = true;
};

// The RGBA a simulation pixel shows as: see `FrameView`.
std::array<std::uint8_t, 4> displayPixel(const std::uint8_t* pixel);

// Truecolor feedback picture: red, green, and blue per pixel, warped alike, as the original's
// 32-bit mode. Metal receives only the RGBA view.
class Engine {
public:
    explicit Engine(std::uint32_t seed);

    void resize(int width, int height);
    void setPaused(bool paused);
    void setLocked(bool locked);
    void nextVisual();
    // Lets the host warp and build maps on several threads. The picture is the same either way.
    void setBandRunner(const BandRunner& bands) { bands_ = bands; }

    bool paused() const { return paused_; }
    bool locked() const { return locked_; }
    int mapMode() const { return mapParameters_.mode; }
    const MapParameters& map() const { return mapParameters_; }
    int waveform() const { return waveform_; }
    const EffectSet& effects() const { return effects_; }
    // The look's slide shift and its current drift.
    const SlideShift& slideShift() const { return slideShift_; }
    // Allows or stops the slide shift for the current look. Each new look rolls it again.
    void setSlideShiftAllowed(bool allowed) { slideShift_.startLook(allowed); }
    // Whether the latest unpaused tick had sound. The effects rest while it is false.
    bool soundActive() const { return soundActive_; }
    // The idle wave: in silence, or sound whose line would draw dimmer than it, after a short
    // wait, a faint made-up signal drawn with the look's waveform keeps the picture moving.
    // Rises to 1, and louder sound fades it back to 0 within half a second.
    float idleLevel() const { return static_cast<float>(idleLevel_); }
    // On by default. A host that wants the picture to fade out in silence turns it off.
    void setIdleWave(bool enabled) { idleWaveEnabled_ = enabled; }
    int width() const { return width_; }

    // Replaces the current look's effects, and rebuilds the map when mode 5 depends on
    // the nuclide. Each new look picks its own again.
    void setEffects(const EffectSet& effects);
    int height() const { return height_; }

    // The slow color drift of the wave and the nuclide, picked once per engine.
    const ColorDrift& colorDrift() const { return drift_; }

    // `dt` is seconds. Gaps longer than 0.1s count as 0.1s.
    // The returned pointer is valid until the next tick or resize.
    FrameView tick(double dt, const AudioFrame& audio);

private:
    void pickLook();
    void pickEffects();
    void rebuildMap();
    void expandPixels(const std::uint8_t* pixels);
    void updateIdle(double dt);
    void drawIdleWave();

    int width_ = 0;
    int height_ = 0;
    bool paused_ = false;
    bool locked_ = false;
    // Opens on mode 25 and waveform 1. Next and the timer replace the pair.
    int waveform_ = 1;
    double mapElapsed_ = 0;
    // Seconds the picture has been moving. Pause holds it. Lock does not.
    double motionTime_ = 0;
    ColorDrift drift_{};
    // Whether `rgba_` changed since the last view was returned.
    bool frameChanged_ = true;
    BandRunner bands_{};
    MapParameters mapParameters_{};
    Random random_;
    EffectSet effects_{};
    EffectLayer effectLayer_;
    SlideShift slideShift_;
    // Changes the warp's rounding pattern every tick.
    std::uint32_t ditherSeed_ = 0;
    // Whether the latest audio had sound. Effects for a new look are picked with it.
    bool soundActive_ = false;
    // Seconds of silence, or of sound whose line would draw dimmer than the idle wave.
    // The idle wave waits on this.
    double faintSeconds_ = 0;
    bool idleWaveEnabled_ = true;
    // 0 with sound, rising to 1 after the idle wait. See `idleLevel`.
    double idleLevel_ = 0;
    // The idle wave's made-up signal, one value per column, refilled each tick it draws.
    AudioFrame idleFrame_{};
    std::vector<SourcePoint> map_;
    // kPixelBytes per pixel.
    std::vector<std::uint8_t> front_;
    std::vector<std::uint8_t> back_;
    std::vector<std::uint8_t> rgba_;
};

}  // namespace geiss
