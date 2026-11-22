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

#import "EngineBridge.h"

#import <QuartzCore/QuartzCore.h>

#include "geiss/audio.hpp"
#include "geiss/engine.hpp"

#include <dispatch/dispatch.h>
#include <simd/simd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

@interface GeissLookInfo ()

- (instancetype)initWithEngine:(const geiss::Engine &)engine NS_DESIGNATED_INITIALIZER;

@end

namespace {

struct BandCall {
    void* job;
    geiss::BandRunner::Band band;
};

void runBand(void* context, size_t index) {
    const auto& call = *static_cast<const BandCall*>(context);
    call.band(call.job, static_cast<int>(index));
}

// Spreads the engine's row bands over the cores. dispatch_apply returns once every band is done,
// and the calling main thread takes a share of them.
void runBandsConcurrently(void* /*host*/, int count, void* job, geiss::BandRunner::Band band) {
    BandCall call{job, band};
    dispatch_apply_f(static_cast<size_t>(count), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), &call,
                     runBand);
}

// Matches WarpParams in Shaders.metal.
struct WarpParams {
    int width;
    int height;
    float step;
    float decay;
    float shiftX;
    float shiftY;
    uint32_t ditherSeed;
};

constexpr int kCheckInterval = 120;

}  // namespace

// The GPU side of the simulation: the warp and expansion kernels, and the engine's frame
// memory wrapped as Metal buffers without a copy.
@interface GeissGPU : NSObject

- (nullable instancetype)initWithLibrary:(id<MTLLibrary>)library queue:(id<MTLCommandQueue>)queue;
// Wraps the engine's buffers again when a resize made new ones.
- (void)adopt:(const geiss::EngineBuffers &)buffers;
// Runs one warp and returns when it is done.
- (void)warp:(const geiss::WarpJob &)job;
- (BOOL)expand:(const uint8_t *)pixels width:(int)width height:(int)height into:(id<MTLTexture>)texture;
// Waits until the last expansion has finished reading the frame.
- (void)waitForReaders;

@property (nonatomic) BOOL check;
// With `check`, adds one tick's main-thread time, printed with the next warp check.
- (void)recordTick:(double)milliseconds;
// With `check`, the latest audio's peak and whether it counted as silence, printed with the
// next warp check, so a check run also shows whether the tap hears anything.
- (void)recordAudioPeak:(float)peak silent:(BOOL)silent;

@end

@implementation GeissGPU {
    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    id<MTLComputePipelineState> warp_;
    id<MTLComputePipelineState> expand_;
    id<MTLBuffer> pixelsA_;
    id<MTLBuffer> pixelsB_;
    id<MTLBuffer> map_;
    const uint8_t* addressA_;
    const uint8_t* addressB_;
    uint64_t generation_;
    BOOL adopted_;
    id<MTLCommandBuffer> lastReader_;
    int warps_;
    std::vector<uint8_t> reference_;
    double tickMilliseconds_;
    int ticks_;
    float peak_;
    BOOL silent_;
}

- (void)recordAudioPeak:(float)peak silent:(BOOL)silent {
    peak_ = std::max(peak_, peak);
    silent_ = silent;
}

- (void)recordTick:(double)milliseconds {
    tickMilliseconds_ += milliseconds;
    ++ticks_;
}

- (instancetype)initWithLibrary:(id<MTLLibrary>)library queue:(id<MTLCommandQueue>)queue {
    self = [super init];
    if (self == nil) {
        return nil;
    }
    id<MTLDevice> device = library.device;
    device_ = device;
    queue_ = queue;
    id<MTLFunction> warp = [library newFunctionWithName:@"geissWarp"];
    id<MTLFunction> expand = [library newFunctionWithName:@"geissExpand"];
    if (warp == nil || expand == nil) {
        return nil;
    }
    NSError* error = nil;
    warp_ = [device newComputePipelineStateWithFunction:warp error:&error];
    expand_ = [device newComputePipelineStateWithFunction:expand error:&error];
    if (warp_ == nil || expand_ == nil) {
        NSLog(@"Geiss: GPU warp unavailable: %@", error);
        return nil;
    }
    return self;
}

- (id<MTLBuffer>)wrap:(void*)pointer length:(std::size_t)length {
    return [device_ newBufferWithBytesNoCopy:pointer
                                      length:length
                                     options:MTLResourceStorageModeShared
                                 deallocator:nil];
}

- (void)adopt:(const geiss::EngineBuffers &)buffers {
    if (adopted_ && buffers.generation == generation_) {
        return;
    }
    [self waitForReaders];
    pixelsA_ = [self wrap:buffers.pixelsA length:buffers.pixelBytes];
    pixelsB_ = [self wrap:buffers.pixelsB length:buffers.pixelBytes];
    map_ = [self wrap:buffers.map length:buffers.mapBytes];
    addressA_ = buffers.pixelsA;
    addressB_ = buffers.pixelsB;
    generation_ = buffers.generation;
    adopted_ = YES;
}

- (id<MTLBuffer>)bufferFor:(const uint8_t*)pointer {
    if (pointer == addressA_) {
        return pixelsA_;
    }
    return pointer == addressB_ ? pixelsB_ : nil;
}

- (void)dispatch:(id<MTLComputeCommandEncoder>)encoder
        pipeline:(id<MTLComputePipelineState>)pipeline
           width:(int)width
          height:(int)height {
    const NSUInteger across = pipeline.threadExecutionWidth;
    const NSUInteger down = std::max<NSUInteger>(1, pipeline.maxTotalThreadsPerThreadgroup / across);
    [encoder dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(width), static_cast<NSUInteger>(height), 1)
       threadsPerThreadgroup:MTLSizeMake(across, down, 1)];
}

- (void)warp:(const geiss::WarpJob &)job {
    id<MTLBuffer> src = [self bufferFor:job.src];
    id<MTLBuffer> dst = [self bufferFor:job.dst];
    id<MTLCommandBuffer> commands = [queue_ commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    if (src == nil || dst == nil || commands == nil || encoder == nil) {
        [encoder endEncoding];
        geiss::warpPixels(job.src, job.dst, job.width, job.height, job.map, job.step, job.decay, job.shiftX,
                          job.shiftY, job.ditherSeed);
        return;
    }
    const bool checking = _check && (warps_++ % kCheckInterval) == 0;
    if (checking) {
        reference_.resize(static_cast<std::size_t>(job.width) * static_cast<std::size_t>(job.height) * geiss::kPixelBytes);
        geiss::warpPixels(job.src, reference_.data(), job.width, job.height, job.map, job.step, job.decay, job.shiftX,
                          job.shiftY, job.ditherSeed);
    }
    WarpParams params{job.width, job.height, job.step, job.decay, job.shiftX, job.shiftY, job.ditherSeed};
    [encoder setComputePipelineState:warp_];
    [encoder setBuffer:src offset:0 atIndex:0];
    [encoder setBuffer:dst offset:0 atIndex:1];
    [encoder setBuffer:map_ offset:0 atIndex:2];
    [encoder setBytes:&params length:sizeof(params) atIndex:3];
    [self dispatch:encoder pipeline:warp_ width:job.width height:job.height];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    if (checking) {
        int largest = 0;
        std::size_t over = 0;
        for (std::size_t i = 0; i < reference_.size(); ++i) {
            const int difference = std::abs(static_cast<int>(reference_[i]) - static_cast<int>(job.dst[i]));
            largest = std::max(largest, difference);
            over += difference > 1 ? 1 : 0;
        }
        std::printf("Geiss warp check: %dx%d, largest difference %d, %zu bytes above 1, %.3f ms GPU, "
                    "%.3f ms per tick on the main thread, audio peak %.3f%s\n",
                    job.width, job.height, largest, over, (commands.GPUEndTime - commands.GPUStartTime) * 1000.0,
                    ticks_ > 0 ? tickMilliseconds_ / ticks_ : 0.0, peak_, silent_ ? " (silent)" : "");
        tickMilliseconds_ = 0;
        ticks_ = 0;
        peak_ = 0;
        std::fflush(stdout);
    }
}

- (BOOL)expand:(const uint8_t *)pixels width:(int)width height:(int)height into:(id<MTLTexture>)texture {
    id<MTLBuffer> source = [self bufferFor:pixels];
    if (source == nil || texture == nil) {
        return NO;
    }
    id<MTLCommandBuffer> commands = [queue_ commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    if (commands == nil || encoder == nil) {
        return NO;
    }
    simd_int2 size = simd_make_int2(width, height);
    [encoder setComputePipelineState:expand_];
    [encoder setBuffer:source offset:0 atIndex:0];
    [encoder setBytes:&size length:sizeof(size) atIndex:1];
    [encoder setTexture:texture atIndex:0];
    [self dispatch:encoder pipeline:expand_ width:width height:height];
    [encoder endEncoding];
    [commands commit];
    lastReader_ = commands;
    return YES;
}

- (void)waitForReaders {
    [lastReader_ waitUntilCompleted];
    lastReader_ = nil;
}

@end

namespace {

void runWarpOnGPU(void* host, const geiss::WarpJob& job) {
    [(__bridge GeissGPU*)host warp:job];
}

}  // namespace

@implementation GeissLookInfo

- (instancetype)initWithEngine:(const geiss::Engine &)engine {
    self = [super init];
    if (self != nil) {
        const geiss::EffectSet &set = engine.effects();
        _mapMode = engine.mapMode();
        _mapName = @(geiss::mapName(engine.mapMode()));
        _waveform = engine.waveform();
        _waveformName = @(geiss::waveformName(engine.waveform()));
        _waveformHidden = set.hideWaveform;
        NSMutableArray<NSString *> *names = [NSMutableArray array];
        for (int index = 0; index < geiss::kEffectCount; ++index) {
            const auto effect = static_cast<geiss::Effect>(index);
            if (!set.has(effect)) {
                continue;
            }
            NSString *name = @(geiss::effectName(effect));
            if (effect == geiss::Effect::Chasers && set.chasers > 1) {
                name = [name stringByAppendingFormat:@" x%d", set.chasers];
            }
            [names addObject:name];
        }
        if (geiss::modeHasEdges(engine.mapMode())) {
            [names addObject:@"Edges"];
        }
        _effects = [names copy];
        _resting = !engine.soundActive();
    }
    return self;
}

@end

@implementation GeissBridge {
    std::unique_ptr<geiss::Engine> engine_;
    geiss::Analyzer analyzer_;
    geiss::AudioFrame frame_;
    float gain_;
    GeissGPU* gpu_;
    const uint8_t* shown_;
    int shownWidth_;
    int shownHeight_;
}

- (instancetype)initWithSeed:(uint32_t)seed {
    self = [super init];
    if (self != nil) {
        engine_ = std::make_unique<geiss::Engine>(seed);
        engine_->setBandRunner(geiss::BandRunner{runBandsConcurrently, nullptr});
        gain_ = 1;
    }
    return self;
}

- (void)setSensitivity:(float)sensitivity {
    gain_ = sensitivity < 0 ? 0 : sensitivity;
}

- (void)setPaused:(BOOL)paused {
    engine_->setPaused(paused);
}

- (void)setLocked:(BOOL)locked {
    engine_->setLocked(locked);
}

- (void)nextVisual {
    engine_->nextVisual();
}

- (void)resizeWidth:(int)width height:(int)height {
    [gpu_ waitForReaders];
    engine_->resize(width, height);
    if (gpu_ != nil) {
        [gpu_ adopt:engine_->buffers()];
    }
}

- (BOOL)useGPUWithLibrary:(id<MTLLibrary>)library queue:(id<MTLCommandQueue>)queue check:(BOOL)check {
    GeissGPU* gpu = [[GeissGPU alloc] initWithLibrary:library queue:queue];
    if (gpu == nil) {
        return NO;
    }
    gpu.check = check;
    gpu_ = gpu;
    [gpu_ adopt:engine_->buffers()];
    engine_->setWarpRunner(geiss::WarpRunner{runWarpOnGPU, (__bridge void*)gpu_});
    engine_->setHostExpands(true);
    return YES;
}

- (BOOL)usesGPU {
    return gpu_ != nil;
}

- (BOOL)expandIntoTexture:(id<MTLTexture>)texture {
    if (gpu_ == nil || shown_ == nullptr) {
        return NO;
    }
    return [gpu_ expand:shown_ width:shownWidth_ height:shownHeight_ into:texture];
}

- (void)ingestInterleaved:(const float *)samples
                frameCount:(int)frameCount
                  channels:(int)channels
                sampleRate:(double)sampleRate
                      time:(double)time {
    geiss::PcmBlock block;
    block.samples = samples;
    block.frameCount = frameCount;
    block.channels = channels;
    block.sampleRate = sampleRate;
    block.time = time;
    frame_ = analyzer_.analyze(block, engine_->width(), gain_);
}

- (GeissLookInfo *)lookInfo {
    return [[GeissLookInfo alloc] initWithEngine:*engine_];
}

- (const uint8_t *)tick:(double)dt width:(int *)width height:(int *)height changed:(BOOL *)changed {
    // The engine draws into the frame the last expansion read, so that must be done first.
    const CFTimeInterval start = CACurrentMediaTime();
    [gpu_ waitForReaders];
    const geiss::FrameView view = engine_->tick(dt, frame_);
    if (gpu_.check) {
        [gpu_ recordTick:(CACurrentMediaTime() - start) * 1000.0];
        [gpu_ recordAudioPeak:frame_.peak silent:frame_.silent ? YES : NO];
    }
    shown_ = view.pixels;
    shownWidth_ = view.width;
    shownHeight_ = view.height;
    if (changed != nullptr) {
        *changed = view.changed ? YES : NO;
    }
    if (width != nullptr) {
        *width = view.width;
    }
    if (height != nullptr) {
        *height = view.height;
    }
    return view.rgba;
}

@end
