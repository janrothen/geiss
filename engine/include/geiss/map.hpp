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

#include "geiss/bands.hpp"
#include "geiss/buffers.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace geiss {

// xorshift32, so the engine does not depend on a global rand(). A zero state would
// stay zero forever, so next() replaces it with a fixed nonzero value first.
struct Random {
    std::uint32_t state = 1;

    std::uint32_t next();
    // 0...1, like the original's (rand()%1000)*0.001.
    float unit();
    // 0...count-1, like the original's rand()%count.
    int below(int count);
};

// One push center of mode 6, the vortex field. Position is a fraction of the frame.
// `type` 0 pushes along (dirX, dirY), 1 swirls one way, 2 swirls the other.
struct VortexPush {
    float xNorm = 0.5f;
    float yNorm = 0.5f;
    float dirX = 0;
    float dirY = 0;
    int type = 0;
};

// One Geiss map, from GenerateChunkOfNewMap. `mode` is its number, 1...25 except 12.
// Centers are fractions of the frame so a resize keeps the look.
struct MapParameters {
    int mode = 25;
    float centerXNorm = 0.5f;
    float centerYNorm = 0.5f;
    // turn1 and scale1. Modes that compute their scale per pixel ignore `scale`.
    float turn = 0;
    float scale = 1;
    // turn2 and scale2, used on alternate pixels by the rotation-dither modes 1, 9, and 11.
    float turn2 = 0;
    float scale2 = 1;
    // Mode 25 only. The source uses the constant 3. A new zoom picks about 2...6.
    float zoom = 3;
    // f1...f3 from the original, per mode.
    float f1 = 0;
    float f2 = 0;
    float f3 = 0;
    // mode_motion_dampened: modes 1, 2, 4, 5, and 7...16 move half as far per frame.
    float damping = 1;
    // Mode 7's per-pixel jitter, standing in for the original's rand_array.
    std::uint32_t noiseSeed = 1;
    // Mode 6's push centers. The original creates ten and uses five.
    std::array<VortexPush, 5> pushes{};
    // Mode 5 only: whether the look has the nuclide effect. Without it the scale falls off
    // with the square root of the radius.
    bool nuclide = true;
};

struct SourcePoint {
    float x = 0;
    float y = 0;
};

constexpr int kFirstMapMode = 1;
constexpr int kLastMapMode = 25;
// The original's sideways splitter, which the port leaves out (user decision).
constexpr int kRetiredMapMode = 12;

// Whether `mode` is one of the maps the port draws. Mode 12, the original's sideways splitter,
// is left out (user decision); the others keep the original's numbers.
bool isMapMode(int mode);

// The map's name from docs/glossary.md, or "?" for a number that is not a map mode.
const char* mapName(int mode);

// The per-map random setup in GenerateChunkOfNewMap for `mode`: center, turn, scale,
// f1...f3, and the mode's own extras. `width` and `height` place the center. A number outside
// 1...25 is clamped into it, and the retired mode 12 gives the smooth zoom, mode 25.
MapParameters randomMapParameters(int mode, int width, int height, Random& random);

// Full-step source for one pixel of GenerateChunkOfNewMap.
// dx and dy are the pixel offset from the map center, and r640 is their length on a
// 640-wide reference, as the original's rmult made it. Most modes compute a scale and
// read from center + rotate(dx, dy) * scale; modes 1, 9, and 11 alternate two
// rotations by pixel parity; modes 6 and 10 use custom motion vectors.
//   1: scale1 constant.                 2: scale1 constant, above 1 (outward).
//   3: 0.95 - dy*(480/H)*0.0005.        4: 0.9 + r640*0.0025*0.14 (sphere).
//   5: f2 - f1*r640/200*1.7 with the nuclide, f2 - f1*sqrt(r640/200) without.
//   6: weighted pushes and swirls.
//   7: f1 - r640*f2 + jitter (fuzzy).   8: 0.85 + 0.1*sin(sqrt(r640)*f1) (ripples).
//   9: f1 - r640*f2, dithered with scale2 (petals).
//  10: x stretched from the center, y read from y*1.04 (scroll).
//  11: two rotations dithered.
//  13: (1.04 - r640^1.5*0.00025*0.14 - 1)*f1 + 1 (black hole).
//  14: 0.9 + 0.2*cos(dy*12/H) (split world).
//  15: f2 + f3*sin(angle*f1) (petals).  16: 1.05 - r640^2*0.00025*0.09, at least -1.5 (crystal ball).
// Modes 17 and up use nx and ny, the offset times 2/width:
//  17: 0.97 - ny^2*0.40 (horizontal tunnel).   18: 0.97 - nx^2*0.40 (vertical tunnel).
//  19: 1.04 - 0.25*length (vortex).            20: 1.15 - sqrt(ny + 1.4)*0.20 (terra).
//  21: 0.95 - int(|nx|*10)*0.03 - int(|ny|*10)*0.03 (diced).
//  22: 0.95 - int(length*10)*0.04 (phonic rings).
//  23: 0.95 - (int(length*20) % 4)*0.12 (fast rings).
//  24: scale 0.96, turn 0.05 (fast swirl).      25: zoom / (zoom + length) (smooth zoom).
SourcePoint mapSource(int x, int y, int width, int height, const MapParameters& map);

// Mode 25 only. Kept so the zoom formula can be checked on its own.
SourcePoint mode25Source(int x, int y, int width, int height, const MapParameters& map);

// `bands` may build the rows at the same time. The result does not depend on it.
void buildMap(std::vector<SourcePoint>& out, int width, int height, const MapParameters& map,
              const BandRunner& bands = {});
// The same into `out`, which must hold width * height points.
void buildMap(SourcePoint* out, int width, int height, const MapParameters& map, const BandRunner& bands = {});

// Bytes per simulation pixel: red, green, blue, and the held level, so rows stay aligned and
// the presenter's expansion is one pass. The original's truecolor buffer was laid out the
// same way, in blue, green, red order, with its fourth byte unused.
// The held level is how much of the pixel's brightness, on the channels' scale, came from a
// fixed texture (mode 6's edges, the grid, crystals) that is held to SDR. It warps and darkens
// with the colors, so a texture's trail keeps its share; see `displayPixel`.
constexpr int kHeldByte = 3;
constexpr int kPixelBytes = 4;

// Samples `src` through `map`, each of red, green, and blue with the same bilinear weights,
// as the original's truecolor warp did. `step` is 0...1 from the pixel toward the map target.
// `decay` multiplies the sample. `src` and `dst` hold kPixelBytes per pixel and must not
// overlap. The held level warps the same way as the colors.
// `shiftX` and `shiftY` move every source point by that many pixels, as the original's
// slide shift (slider1) offset the source pointer.
// Each result rounds up or down at random, in proportion to its fraction, so a frame's
// small darkening is kept on average instead of rounding away. A pixel's three channels share
// one threshold, so the rounding never speckles its hue, and the held level shares it too.
// Pass a different `ditherSeed` each frame so the pattern changes.
// `bands` may warp the rows at the same time. The result does not depend on it.
void warpPixels(const std::uint8_t* src, std::uint8_t* dst, int width, int height,
                const SourcePoint* map, float step, float decay, float shiftX = 0, float shiftY = 0,
                std::uint32_t ditherSeed = 0, const BandRunner& bands = {});

// One warp of a whole frame, as `warpPixels` takes it. The engine passes `step` and `decay`
// already clamped to 0...1, as `warpPixels` would clamp them.
struct WarpJob {
    const std::uint8_t* src = nullptr;
    std::uint8_t* dst = nullptr;
    int width = 0;
    int height = 0;
    const SourcePoint* map = nullptr;
    float step = 1;
    float decay = 1;
    float shiftX = 0;
    float shiftY = 0;
    std::uint32_t ditherSeed = 0;
};

// Lets a host run the engine's warp elsewhere, such as on a GPU. Without a host the engine
// calls `warpPixels` with its band runner.
// Without `wait`, `run` must leave `job.dst` complete when it returns, as `warpPixels` would
// have written it. With `wait`, `run` may return at once and finish the warp later: the engine
// calls `wait` before it touches `job.src`, `job.dst`, or `job.map` again, and `wait` returns
// with `job.dst` complete. Such a host can let the engine start each frame's warp at the end
// of the frame before (see `Engine::startNextWarp`).
struct WarpRunner {
    using Run = void (*)(void* host, const WarpJob& job);
    using Wait = void (*)(void* host);

    Run run = nullptr;
    void* host = nullptr;
    Wait wait = nullptr;
};

// Wraps a source column into 0...width-1, as the warp does: the original's newx stepped by
// FXW-1 until it was in range.
float wrapColumn(float value, int width);

// The rounding threshold, 0...1, that `warpPixels` uses for one pixel and seed.
float ditherThreshold(int x, int y, std::uint32_t seed);

// Original weightsum numerator for this area. The per-frame multiplier at 30 fps is numerator/256.
int weightNumerator(int width, int height);

}  // namespace geiss
