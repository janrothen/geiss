// This file is part of the Geiss macOS port.
//
// Derived from Geiss HDR, copyright 2026 Ryan Geiss, www.geisswerks.com/geiss_hdr
// Licensed under the Apache License, Version 2.0; see engine/third_party/geiss_hdr/LICENSE.txt.
// Attribution and naming notice: see engine/third_party/geiss_hdr/NOTICE.txt.
//
// Palettes and tone mapping ported from Geiss HDR's palette.js and engine.js.

#pragma once

#include <array>

namespace geiss {

constexpr int kMaxPaletteStops = 8;
constexpr int kPaletteLutSize = 256;
// Geiss HDR caps palette colors at three times SDR white, so the brightest parts do not hurt
// the eyes on a screen with more headroom.
constexpr float kPaletteMaxValue = 3.0f;

// Linear light: 0 is black and 1 is SDR white. Above 1 is HDR.
struct LinearRgb {
    float r = 0;
    float g = 0;
    float b = 0;
};

// A hand-tuned gradient from black up to its brightest color, evenly spaced. Stop 0 holds
// the palette's companion background color in Geiss HDR; the ramp always reads it as black.
struct Palette {
    int count = 0;
    std::array<LinearRgb, kMaxPaletteStops> stops{};
};

// Geiss HDR's saved palettes.
int paletteCount();
// Clamped to the table.
const Palette& savedPalette(int index);

// The ramp at t in [0, 1], linearly interpolated, with stop 0 as black (ExpandPalette).
LinearRgb samplePalette(const Palette& palette, float t);

// Geiss HDR's HdrToSdr: brings a color at ramp position t down to SDR (every channel at most 1),
// given the brightest channel in the whole ramp. It mostly darkens, keeping the channel ratios,
// fades the top of a very bright ramp toward white, lifts midtones, and darkens deep shadows.
LinearRgb hdrToSdr(float t, LinearRgb color, float maxRgbInPalette);

// Geiss HDR's ReduceHdrHeadroom: the same remap into [0, headroom] instead of [0, 1].
LinearRgb reduceHdrHeadroom(float headroom, float t, LinearRgb color, float maxRgbInPalette);

// One linear RGBA entry per index, alpha 1. A presenter interpolates between entries.
using PaletteLut = std::array<std::array<float, 4>, kPaletteLutSize>;

// The look-up table a presenter maps brightness through: palette `from` blended toward `to` by
// `blend` (0...1), capped at kPaletteMaxValue, and remapped into the screen's headroom
// (1 on an SDR screen), as Geiss HDR built its uploaded palette.
void buildPaletteLut(const Palette& from, const Palette& to, float blend, float headroom, PaletteLut& out);

}  // namespace geiss
