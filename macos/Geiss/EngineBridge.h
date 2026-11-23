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

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN

// What the current look is made of, for the debug info readout.
@interface GeissLookInfo : NSObject

- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;

@property (nonatomic, readonly) int mapMode;
@property (nonatomic, readonly, copy) NSString *mapName;
@property (nonatomic, readonly) int waveform;
@property (nonatomic, readonly, copy) NSString *waveformName;
// True when the nuclide or spectral dots replace the waveform in this look.
@property (nonatomic, readonly) BOOL waveformHidden;
// The look's effects in the original's order. Chasers reads "Chasers x2" for two. Mode 6
// adds "Edges", which belong to the map rather than the effect picks.
@property (nonatomic, readonly, copy) NSArray<NSString *> *effects;
// True while the engine last ticked in silence, so the effects rest, apart from the
// nuclide's random rings.
@property (nonatomic, readonly) BOOL resting;
// The Geiss HDR palette the look fades to, and how far that fade has come, 0 to 1.
@property (nonatomic, readonly) int palette;
@property (nonatomic, readonly) float paletteBlend;

@end

// The SwiftUI shell sees only these Objective-C types. The pointer from tick is
// valid until the next tick or resize. `changed` is NO when the pixels are the same as the
// previous tick's, for example while paused, so the presenter can skip the upload. On the GPU
// path tick returns NULL and the presenter calls expandIntoTexture: instead.
@interface GeissBridge : NSObject

- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (instancetype)initWithSeed:(uint32_t)seed NS_DESIGNATED_INITIALIZER;
- (void)setSensitivity:(float)sensitivity;
- (void)setPaused:(BOOL)paused;
- (void)setLocked:(BOOL)locked;
- (void)nextVisual;
- (void)resizeWidth:(int)width height:(int)height;
// Moves the warp and the expansion to RGBA onto the GPU, sharing the engine's frame memory
// without a copy. `library` holds the geissWarp and geissExpand kernels (Shaders.metal). Work is encoded on `queue`, the presenter's, so an expansion runs before the
// draw that reads it. With `check`, every 120th warp also runs on the CPU and the largest
// difference is logged. Returns NO, and stays on the CPU, if the kernels cannot be loaded.
- (BOOL)useGPUWithLibrary:(id<MTLLibrary>)library queue:(id<MTLCommandQueue>)queue check:(BOOL)check;
@property (nonatomic, readonly) BOOL usesGPU;
// GPU path only: expands the latest frame into `texture` (rgba8Unorm, shader-writable, the size
// of the frame) and commits that work. prepareNextFrame and the next tick wait for it before
// drawing into the frame again. Returns NO when there is no frame to expand.
- (BOOL)expandIntoTexture:(id<MTLTexture>)texture;
// GPU path only: call after presenting each tick's frame. Waits for its expansion, draws the
// next frame's effects, and starts the next frame's warp, so the GPU warps between frames
// and the next tick only waits for a finished warp. Does nothing on the CPU path.
- (void)prepareNextFrame;
- (void)ingestInterleaved:(const float * _Nullable)samples
                frameCount:(int)frameCount
                  channels:(int)channels
                sampleRate:(double)sampleRate
                      time:(double)time;
- (GeissLookInfo *)lookInfo;
// Entries in the palette table copyPaletteForHeadroom:into: writes, four floats each.
@property (class, nonatomic, readonly) int paletteEntries;
// Writes the current palette table, adapted to `headroom` (1 on an SDR screen), into `lut`:
// paletteEntries linear RGBA floats. The presenter maps each pixel's brightness through it.
- (void)copyPaletteForHeadroom:(float)headroom into:(float *)lut;
- (const uint8_t * _Nullable)tick:(double)dt
                  width:(int *)width
                 height:(int *)height
                changed:(BOOL *)changed;

@end

NS_ASSUME_NONNULL_END
