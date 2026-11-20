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
#include "geiss/effects.hpp"
#include "geiss/map.hpp"
#include "geiss/palette.hpp"
#include "geiss/shift.hpp"

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

// False for the map and waveform pairs the original re-rolls, and for unknown numbers.
bool lookAllowed(int mode, int waveform);

// The original's pick: a uniform mode with extra chances for 7 and 5, and a waveform
// with 6 less likely, re-rolled until the pair is allowed.
Look randomLook(Random& random);

struct FrameView {
    const std::uint8_t* rgba = nullptr;
    int width = 0;
    int height = 0;
};

// Index-buffer feedback picture. Metal receives only the RGBA view.
class Engine {
public:
    explicit Engine(std::uint32_t seed);

    void resize(int width, int height);
    void setPaused(bool paused);
    void setLocked(bool locked);
    void nextVisual();

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
    int width() const { return width_; }

    // Replaces the current look's effects, and rebuilds the map when mode 5 depends on
    // the nuclide. Each new look picks its own again.
    void setEffects(const EffectSet& effects);
    int height() const { return height_; }

    // The current look's palette. Each new look picks its own and blends to it.
    const PaletteRecipe& palette() const { return paletteRecipe_; }
    // The colors on screen, partway through a blend while one runs.
    const Palette& paletteColors() const { return palette_; }
    // Replaces the palette at once, without a blend. Out-of-range curves and orders are clamped.
    void setPalette(const PaletteRecipe& recipe);

    // `dt` is seconds. Gaps longer than 0.1s count as 0.1s.
    // The returned pointer is valid until the next tick or resize.
    FrameView tick(double dt, const AudioFrame& audio);

private:
    void pickLook();
    void pickEffects();
    void rebuildMap();
    void pickPalette(bool blend);
    bool soundEmpty() const;
    void expandPalette(const std::uint8_t* indices);

    int width_ = 0;
    int height_ = 0;
    bool paused_ = false;
    bool locked_ = false;
    // Opens on mode 25 and waveform 1. Next and the timer replace the pair.
    int waveform_ = 1;
    double mapElapsed_ = 0;
    // Seconds the picture has been moving. Pause holds it. Lock does not.
    double motionTime_ = 0;
    PaletteRecipe paletteRecipe_{};
    Palette palette_{};
    Palette fromPalette_{};
    Palette targetPalette_{};
    // Seconds left in the blend from `fromPalette_` to `targetPalette_`.
    double paletteBlendLeft_ = 0;
    MapParameters mapParameters_{};
    Random random_;
    EffectSet effects_{};
    EffectLayer effectLayer_;
    SlideShift slideShift_;
    // Whether the latest audio had sound. Effects for a new look are picked with it.
    bool soundActive_ = false;
    // Seconds of unbroken silence. A palette picked after a long silence is brighter.
    double silentSeconds_ = 0;
    std::vector<SourcePoint> map_;
    std::vector<std::uint8_t> front_;
    std::vector<std::uint8_t> back_;
    std::vector<std::uint8_t> rgba_;
};

}  // namespace geiss
