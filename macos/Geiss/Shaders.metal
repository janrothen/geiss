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
#include <metal_stdlib>
using namespace metal;

// ---- Drawing ----

struct Varying {
    float4 position [[position]];
    float2 uv;
};

vertex Varying geissVertex(uint id [[vertex_id]]) {
    const float2 xy[4] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };
    const float2 uv[4] = { float2(0, 1), float2(1, 1), float2(0, 0), float2(1, 0) };
    Varying out;
    out.position = float4(xy[id], 0, 1);
    out.uv = uv[id];
    return out;
}

// geiss::kPaletteLutSize.
constant int kPaletteEntries = 256;

// The palette table at t in [0, 1], interpolated between entries, as Geiss HDR read its palette.
float3 paletteColor(constant float4* lut, float t) {
    const float x = saturate(t) * float(kPaletteEntries - 1);
    const int i = min(int(x), kPaletteEntries - 2);
    return mix(lut[i].rgb, lut[i + 1].rgb, x - float(i));
}

// RGB is the simulated color as stored, sampled raw. Its brightest channel, times `gain`, picks a color from
// the palette table, which the engine has already remapped into the screen's headroom, so the
// top of the palette glows on an HDR screen. Alpha is the share of that brightness held to SDR:
// fixed textures (mode 6's edges, the grid, crystals) keep that share at or below SDR white.
fragment float4 geissFragment(Varying in [[stage_in]], texture2d<float> image [[texture(0)]],
                              constant float4* lut [[buffer(0)]], constant float& gain [[buffer(1)]]) {
    constexpr sampler linear(filter::linear, address::clamp_to_edge);
    const float4 sampled = image.sample(linear, in.uv);
    const float t = max(sampled.r, max(sampled.g, sampled.b)) * gain;
    const float3 color = paletteColor(lut, t);
    return float4(mix(color, min(color, float3(1.0)), sampled.a), 1.0);
}

// ---- Simulation ----
// The engine's simulation pixels are 4 bytes: red, green, blue, and the held level
// (engine/include/geiss/map.hpp, kPixelBytes and kHeldByte). The buffers are the engine's own
// memory, shared without a copy.

// Matches geiss::WarpJob's numbers, minus the pointers.
struct WarpParams {
    int width;
    int height;
    float step;
    float decay;
    float shiftX;
    float shiftY;
    uint ditherSeed;
};

// geiss::wrapColumn: columns wrap around, as the original's newx stepped by FXW - 1.
float wrapColumn(float value, int width) {
    const float span = float(width - 1);
    if (span <= 0.0) {
        return 0.0;
    }
    if (value >= 0.0 && value < span) {
        return value;
    }
    float wrapped = fmod(value, span);
    if (wrapped < 0.0) {
        wrapped += span;
    }
    return wrapped;
}

// geiss::ditherThreshold: lowbias32 over the pixel and the seed, 0...1 - 2^-24.
float ditherThreshold(int x, int y, uint seed) {
    uint h = uint(x) * 0x9E3779B1u ^ uint(y) * 0x85EBCA77u ^ seed * 0xC2B2AE3Du;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return float(h >> 8) * (1.0 / 16777216.0);
}

// geiss::warpPixels for one pixel (warpBand in engine/src/map.cpp): the source point moves
// `step` of the way to the map's target, columns wrap and rows clamp, and the four
// neighbors' bytes are blended with the same bilinear weights times the decay, then rounded
// up or down at random by one threshold for the whole pixel.
kernel void geissWarp(device const uchar4* src [[buffer(0)]],
                      device uchar4* dst [[buffer(1)]],
                      device const float2* map [[buffer(2)]],
                      constant WarpParams& job [[buffer(3)]],
                      uint2 gid [[thread_position_in_grid]]) {
    const int width = job.width;
    const int height = job.height;
    if (int(gid.x) >= width || int(gid.y) >= height) {
        return;
    }
    const int x = int(gid.x);
    const int y = int(gid.y);
    const uint pixel = uint(y) * uint(width) + uint(x);
    const float2 target = map[pixel];
    const float sx = wrapColumn(float(x) + (target.x - float(x)) * job.step + job.shiftX, width);
    const float sy = clamp(float(y) + (target.y - float(y)) * job.step + job.shiftY, 0.0, float(height - 1));

    const int x0 = clamp(int(sx), 0, width - 1);
    const int y0 = clamp(int(sy), 0, height - 1);
    const int x1 = min(x0 + 1, width - 1);
    const int y1 = min(y0 + 1, height - 1);
    const float fx = clamp(sx - float(x0), 0.0, 1.0);
    const float fy = clamp(sy - float(y0), 0.0, 1.0);

    const float4 p00 = float4(src[uint(y0) * uint(width) + uint(x0)]);
    const float4 p10 = float4(src[uint(y0) * uint(width) + uint(x1)]);
    const float4 p01 = float4(src[uint(y1) * uint(width) + uint(x0)]);
    const float4 p11 = float4(src[uint(y1) * uint(width) + uint(x1)]);
    const float w00 = (1.0 - fx) * (1.0 - fy) * job.decay;
    const float w10 = fx * (1.0 - fy) * job.decay;
    const float w01 = (1.0 - fx) * fy * job.decay;
    const float w11 = fx * fy * job.decay;
    const float4 value = p00 * w00 + p10 * w10 + p01 * w01 + p11 * w11;
    const float threshold = ditherThreshold(x, y, job.ditherSeed);
    dst[pixel] = uchar4(min(value + threshold, float4(255.0)));
}

// geiss::displayPixel: the stored channels, and the share of the brightest one held to SDR.
// Written to the presenter's texture, which the fragment shader samples.
kernel void geissExpand(device const uchar4* pixels [[buffer(0)]],
                        constant int2& size [[buffer(1)]],
                        texture2d<float, access::write> out [[texture(0)]],
                        uint2 gid [[thread_position_in_grid]]) {
    if (int(gid.x) >= size.x || int(gid.y) >= size.y) {
        return;
    }
    const int4 p = int4(pixels[gid.y * uint(size.x) + gid.x]);
    const int brightest = max(p.r, max(p.g, p.b));
    const int held = min(p.a, brightest);
    const int share = brightest > 0 ? held * 255 / brightest : 0;
    out.write(float4(float3(p.rgb), float(share)) * (1.0 / 255.0), gid);
}
