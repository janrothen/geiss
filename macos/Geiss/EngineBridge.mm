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

#include "geiss/audio.hpp"
#include "geiss/engine.hpp"

#include <dispatch/dispatch.h>

#include <memory>

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
    engine_->resize(width, height);
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
    const geiss::FrameView view = engine_->tick(dt, frame_);
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
