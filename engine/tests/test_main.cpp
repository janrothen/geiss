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

#include "geiss/audio.hpp"
#include "geiss/beat.hpp"
#include "geiss/engine.hpp"
#include "geiss/map.hpp"
#include "geiss/palette.hpp"
#include "geiss/shift.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

void checkNear(float actual, float expected, float tolerance, const char* message) {
    if (std::fabs(actual - expected) > tolerance) {
        std::cerr << message << " actual=" << actual << " expected=" << expected << '\n';
        ++failures;
    }
}

void testMode25PullsTowardCenter() {
    geiss::MapParameters map;
    map.centerXNorm = 0.5f;
    map.centerYNorm = 0.5f;
    map.turn = 0;
    map.zoom = 3;
    const geiss::SourcePoint center = geiss::mode25Source(50, 40, 100, 80, map);
    checkNear(center.x, 50.f, 0.01f, "center x stays put");
    checkNear(center.y, 40.f, 0.01f, "center y stays put");

    const geiss::SourcePoint edge = geiss::mode25Source(70, 40, 100, 80, map);
    check(edge.x < 70.f && edge.x > 50.f, "mode 25 looks inward from the right");

    map.zoom = 2;
    const geiss::SourcePoint fast = geiss::mode25Source(70, 40, 100, 80, map);
    map.zoom = 6;
    const geiss::SourcePoint slow = geiss::mode25Source(70, 40, 100, 80, map);
    check(fast.x < slow.x, "a smaller zoom constant pulls harder");
}

void testWarpDecayAndSample() {
    constexpr int width = 4;
    constexpr int height = 2;
    std::uint8_t src[8] = {10, 20, 0, 0, 30, 40, 0, 0};
    std::uint8_t dst[8] = {};
    std::vector<geiss::SourcePoint> map(8);
    for (int i = 0; i < 8; ++i) {
        map[static_cast<std::size_t>(i)] = {0.f, 0.f};
    }
    geiss::warpIndices(src, dst, width, height, map.data(), 1.f, 0.5f);
    check(dst[0] == 5, "identity sample at 0,0 decays by half");
    check(dst[1] == 5, "a target of 0,0 copies the decayed corner");

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            map[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = {static_cast<float>(x), static_cast<float>(y)};
        }
    }
    src[0] = 100;
    geiss::warpIndices(src, dst, width, height, map.data(), 0.f, 0.5f);
    check(dst[0] == 50, "step 0 samples the pixel itself");
}

void testAnalysis() {
    geiss::Analyzer analyzer;
    const float stereo[] = {1.f, -1.f, 1.f, -1.f};
    geiss::PcmBlock block;
    block.samples = stereo;
    block.frameCount = 2;
    block.channels = 2;
    block.sampleRate = 2;
    block.time = 0;
    const geiss::AudioFrame downmix = analyzer.analyze(block, 3, 1.f);
    checkNear(downmix.level, 0.f, 0.0001f, "stereo downmix of opposites is silent");
    check(downmix.samples.size() == 3, "analysis writes one sample per column");

    geiss::Analyzer levels;
    const float constant[] = {0.5f, 0.5f, 0.5f, 0.5f};
    block.samples = constant;
    block.frameCount = 4;
    block.channels = 1;
    block.sampleRate = 4;
    const geiss::AudioFrame gained = levels.analyze(block, 2, 2.f);
    checkNear(gained.level, 1.f, 0.0001f, "gain scales the RMS");
    checkNear(gained.peak, 1.f, 0.0001f, "gain scales the peak");
    check(!gained.silent, "a loud block is not silence");
    check(!gained.beatMode, "one block is not a beat");

    const float quiet[] = {0.f};
    block.samples = quiet;
    block.frameCount = 1;
    block.sampleRate = 4;
    geiss::Analyzer silence;
    const geiss::AudioFrame early = silence.analyze(block, 2, 1.f);
    check(!early.silent, "a quarter second of silence is not the hold yet");
    const geiss::AudioFrame later = silence.analyze(block, 2, 1.f);
    check(later.silent, "silence holds for half a second");
}

std::vector<float> sineBlock(int frames, double sampleRate, double frequency, double phase) {
    std::vector<float> samples(static_cast<std::size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const auto value = static_cast<float>(0.5 * std::sin(phase + 2.0 * std::numbers::pi * frequency * i / sampleRate));
        samples[static_cast<std::size_t>(i) * 2] = value;
        samples[static_cast<std::size_t>(i) * 2 + 1] = value;
    }
    return samples;
}

geiss::AudioFrame analyzeSine(int frames, double phase, int columns) {
    const std::vector<float> samples = sineBlock(frames, 48000.0, 220.0, phase);
    geiss::PcmBlock block;
    block.samples = samples.data();
    block.frameCount = frames;
    block.channels = 2;
    block.sampleRate = 48000.0;
    geiss::Analyzer analyzer;
    return analyzer.analyze(block, columns, 1.f);
}

int risingCrossings(const std::vector<float>& columns) {
    int count = 0;
    for (std::size_t i = 1; i < columns.size(); ++i) {
        if (columns[i - 1] <= 0.f && columns[i] > 0.f) {
            ++count;
        }
    }
    return count;
}

void testWaveWindow() {
    // 960 columns span 960 / 88200 s, about 10.9 ms, so a 220 Hz tone shows about 2.4 cycles.
    const geiss::AudioFrame ring = analyzeSine(2048, 0.0, 960);
    const geiss::AudioFrame frame = analyzeSine(800, 0.0, 960);
    check(risingCrossings(ring.samples) == 2, "a 220 Hz tone shows about two cycles");
    check(risingCrossings(frame.samples) == risingCrossings(ring.samples), "block length does not change the span");
    check(ring.samples.front() >= -0.01f && ring.samples.front() < 0.05f && ring.samples[1] > ring.samples.front(),
          "the window starts at a rising edge");

    // A later block that ends mid-cycle draws the same line.
    const geiss::AudioFrame later = analyzeSine(2048, 1.3, 960);
    float worst = 0.f;
    for (std::size_t i = 0; i < ring.samples.size(); ++i) {
        worst = std::max(worst, std::fabs(later.samples[i] - ring.samples[i]));
    }
    check(worst < 0.05f, "the trigger holds the line in place");
    check(later.left == later.samples && later.right == later.samples, "left and right share the window");

    // A block shorter than the window spans all of it.
    const geiss::AudioFrame tiny = analyzeSine(100, 0.0, 960);
    check(tiny.samples.size() == 960, "a short block still fills every column");
}

void testEnginePauseDrawsALine() {
    geiss::Engine engine(1);
    engine.resize(32, 24);
    engine.setEffects({});
    geiss::AudioFrame audio;
    audio.level = 0.5f;
    audio.volume = 100.f;
    audio.averageVolume = 100.f;
    audio.silent = false;
    audio.samples.resize(32);
    for (int i = 0; i < 32; ++i) {
        audio.samples[static_cast<std::size_t>(i)] = std::sin(static_cast<float>(i) * 0.4f);
    }
    const geiss::FrameView frame = engine.tick(1.0 / 30.0, audio);
    check(frame.width == 32 && frame.height == 24, "engine reports the simulation size");
    unsigned color = 0;
    const std::size_t pixels = static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    for (std::size_t i = 0; i < pixels; ++i) {
        color += frame.rgba[i * 4];
        color += frame.rgba[i * 4 + 1];
        color += frame.rgba[i * 4 + 2];
    }
    check(color > 0, "a tone leaves color in the frame");

    std::vector<std::uint8_t> frozen(frame.rgba, frame.rgba + pixels * 4);
    engine.setPaused(true);
    const geiss::FrameView held = engine.tick(1.0, audio);
    check(frozen.size() == static_cast<std::size_t>(held.width) * static_cast<std::size_t>(held.height) * 4, "pause keeps the size");
    bool same = true;
    for (std::size_t i = 0; i < frozen.size(); ++i) {
        same = same && frozen[i] == held.rgba[i];
    }
    check(same, "pause returns the same picture");
}

std::vector<float> stereoTone(double frequency, int frames, double sampleRate, float amplitude) {
    std::vector<float> out(static_cast<std::size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const auto value = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * frequency * i / sampleRate));
        out[static_cast<std::size_t>(i) * 2] = value;
        out[static_cast<std::size_t>(i) * 2 + 1] = value;
    }
    return out;
}

void testBassLevel() {
    constexpr double rate = 48000;
    constexpr int frames = 4800;
    const std::vector<float> low = stereoTone(50, frames, rate, 0.5f);
    const std::vector<float> high = stereoTone(2000, frames, rate, 0.5f);
    const float lowLevel = geiss::bassLevel(low.data(), frames, 2, rate, 1.f);
    const float highLevel = geiss::bassLevel(high.data(), frames, 2, rate, 1.f);
    check(lowLevel > 0.25f, "a 50 Hz tone passes the bass filter");
    check(highLevel < lowLevel * 0.02f, "a 2 kHz tone is filtered out");
    checkNear(geiss::bassLevel(low.data(), frames, 2, rate, 2.f), lowLevel * 2.f, 0.0001f, "gain scales the bass level");
    checkNear(geiss::bassLevel(nullptr, frames, 2, rate, 1.f), 0.f, 0.f, "no samples has no bass");
    checkNear(geiss::bassLevel(low.data(), frames, 2, 0, 1.f), 0.f, 0.f, "no sample rate has no bass");
}

// Bass that hits for 50 ms twice a second, over a quiet floor.
float kick(double time) {
    return std::fmod(time, 0.5) < 0.05 ? 0.5f : 0.02f;
}

struct BeatRun {
    bool beatModeAtEnd = false;
    float minBrightness = 1;
    float maxBrightness = 0;
};

BeatRun runDetector(float (*bass)(double), double updatesPerSecond, double seconds) {
    geiss::BeatDetector detector;
    BeatRun run;
    const double step = 1.0 / updatesPerSecond;
    const int updates = static_cast<int>(seconds * updatesPerSecond);
    for (int i = 0; i < updates; ++i) {
        const geiss::BeatState state = detector.update(bass(i * step), step);
        run.beatModeAtEnd = state.beatMode;
        if (state.beatMode) {
            run.minBrightness = std::min(run.minBrightness, state.brightness);
            run.maxBrightness = std::max(run.maxBrightness, state.brightness);
        } else {
            check(state.brightness == 1.f, "brightness is full outside beat mode");
        }
    }
    return run;
}

void testBeatDetector() {
    const BeatRun kicks = runDetector(kick, 60, 8);
    check(kicks.beatModeAtEnd, "a steady kick turns beat mode on");
    check(kicks.minBrightness < 0.1f && kicks.maxBrightness > 0.9f, "beat brightness follows the hits");

    check(runDetector(kick, 120, 8).beatModeAtEnd, "beat mode does not depend on the update rate");
    check(runDetector(kick, 24, 8).beatModeAtEnd, "beat mode holds at a slow update rate");

    check(!runDetector([](double) { return 0.3f; }, 60, 8).beatModeAtEnd, "steady bass is not a beat");
    check(!runDetector([](double t) { return kick(t) * 0.01f; }, 60, 8).beatModeAtEnd, "a beat under the gate is ignored");

    geiss::BeatDetector detector;
    for (int i = 0; i < 120; ++i) {
        detector.update(0.1f, 1.0 / 60.0);
    }
    const geiss::BeatState steady = detector.update(0.1f, 1.0 / 60.0);
    checkNear(steady.bigBeatRatio, 1.f, 0.01f, "steady bass has a big-beat ratio of 1");
    const geiss::BeatState jump = detector.update(0.3f, 1.0 / 60.0);
    check(jump.bigBeatRatio > 1.5f, "a jump in bass is a big beat");
    geiss::BeatState settled;
    for (int i = 0; i < 120; ++i) {
        settled = detector.update(0.3f, 1.0 / 60.0);
    }
    checkNear(settled.bigBeatRatio, 1.f, 0.01f, "a louder level that holds is no longer a big beat");
}

void testSilenceHoldUsesTime() {
    // Overlapping 2048-frame snapshots, one per 1/60 s, as the app sends them.
    const std::vector<float> quiet(std::size_t{2048} * 2, 0.f);
    geiss::PcmBlock block;
    block.samples = quiet.data();
    block.frameCount = 2048;
    block.channels = 2;
    block.sampleRate = 48000;
    geiss::Analyzer analyzer;
    bool silentEarly = false;
    for (int i = 0; i < 24; ++i) {
        block.time = 100.0 + i / 60.0;
        silentEarly = silentEarly || analyzer.analyze(block, 8, 1.f).silent;
    }
    check(!silentEarly, "0.4 s of overlapping quiet blocks is not silence yet");
    bool silentLater = false;
    for (int i = 24; i < 40; ++i) {
        block.time = 100.0 + i / 60.0;
        silentLater = analyzer.analyze(block, 8, 1.f).silent;
    }
    check(silentLater, "0.65 s of quiet is silence");
}

geiss::AudioFrame beatFrame(bool beatMode, float brightness, float ratio) {
    geiss::AudioFrame audio;
    audio.level = 0.5f;
    audio.volume = 100.f;
    audio.averageVolume = 100.f;
    audio.samples.assign(32, 0.5f);
    audio.beatMode = beatMode;
    audio.beatBrightness = brightness;
    audio.bigBeatRatio = ratio;
    return audio;
}

unsigned colorSum(const geiss::FrameView& frame) {
    unsigned color = 0;
    const std::size_t pixels = static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    for (std::size_t i = 0; i < pixels; ++i) {
        color += frame.rgba[i * 4] + frame.rgba[i * 4 + 1] + frame.rgba[i * 4 + 2];
    }
    return color;
}

void testBeatBrightness() {
    geiss::Engine dark(1);
    dark.resize(32, 24);
    dark.setEffects({});
    check(colorSum(dark.tick(1.0 / 30.0, beatFrame(true, 0.f, 0.f))) == 0, "a beat-mode gap draws no line");

    geiss::Engine lit(1);
    lit.resize(32, 24);
    lit.setEffects({});
    check(colorSum(lit.tick(1.0 / 30.0, beatFrame(true, 1.f, 0.f))) > 0, "a beat-mode hit draws the line");
}

struct Look {
    int mode;
    int wave;
};

Look lookOf(const geiss::Engine& engine) {
    return {engine.mapMode(), engine.waveform()};
}

bool sameLook(Look a, Look b) {
    return a.mode == b.mode && a.wave == b.wave;
}

// Seconds of 0.1 s ticks until the look changes, or -1 within `limit`.
double secondsUntilLookChanges(geiss::Engine& engine, const geiss::AudioFrame& audio, double limit) {
    const Look start = lookOf(engine);
    for (int i = 1; i <= static_cast<int>(limit * 10); ++i) {
        engine.tick(0.1, audio);
        if (!sameLook(lookOf(engine), start)) {
            return i * 0.1;
        }
    }
    return -1;
}

void testVolumeScale() {
    constexpr double rate = 48000;
    const std::vector<float> loud = stereoTone(440, 4800, rate, 1.f);
    geiss::PcmBlock block;
    block.samples = loud.data();
    block.frameCount = 4800;
    block.channels = 2;
    block.sampleRate = rate;
    geiss::Analyzer analyzer;
    const geiss::AudioFrame full = analyzer.analyze(block, 8, 1.f);
    checkNear(full.volume, 256.f, 1.f, "a full-scale wave is volume 256, as a 16-bit peak-to-peak / 256");
    check(full.averageVolume > 0.f && full.averageVolume < full.volume, "the average volume trails a sudden rise");
    geiss::Analyzer quieter;
    checkNear(quieter.analyze(block, 8, 0.5f).volume, 128.f, 1.f, "gain scales the volume");
}

void testWaveBrightnessFollowsVolume() {
    geiss::AudioFrame quiet = beatFrame(false, 1.f, 0.f);
    quiet.volume = 10.f;
    quiet.averageVolume = 10.f;
    geiss::AudioFrame normal = beatFrame(false, 1.f, 0.f);
    normal.volume = 50.f;
    normal.averageVolume = 50.f;
    geiss::AudioFrame loud = beatFrame(false, 1.f, 0.f);
    loud.volume = 150.f;
    loud.averageVolume = 150.f;

    geiss::Engine quietEngine(1);
    quietEngine.resize(32, 24);
    quietEngine.setEffects({});
    geiss::Engine normalEngine(1);
    normalEngine.resize(32, 24);
    normalEngine.setEffects({});
    geiss::Engine loudEngine(1);
    loudEngine.resize(32, 24);
    loudEngine.setEffects({});
    const unsigned quietColor = colorSum(quietEngine.tick(1.0 / 30.0, quiet));
    const unsigned normalColor = colorSum(normalEngine.tick(1.0 / 30.0, normal));
    const unsigned loudColor = colorSum(loudEngine.tick(1.0 / 30.0, loud));
    check(quietColor > 0 && quietColor < normalColor, "a quiet wave is dimmer");
    check(normalColor == loudColor, "normal listening levels already draw at full brightness");

    geiss::AudioFrame silent = beatFrame(false, 1.f, 0.f);
    silent.volume = 0.f;
    silent.averageVolume = 0.f;
    geiss::Engine silentEngine(1);
    silentEngine.resize(32, 24);
    silentEngine.setEffects({});
    check(colorSum(silentEngine.tick(1.0 / 30.0, silent)) == 0, "no volume draws no line");
}

// Red, green, and blue of palette entry `n`.
std::array<int, 3> entry(const geiss::Palette& palette, int n) {
    const auto i = static_cast<std::size_t>(n) * 3;
    return {palette[i], palette[i + 1], palette[i + 2]};
}

void testPaletteCurves() {
    checkNear(geiss::paletteCurve(1, 100), 226.f, 0.01f, "curve 1 is sqrt(x) * 22.6");
    checkNear(geiss::paletteCurve(2, 50), 100.f, 0.01f, "curve 2 is x * 2");
    checkNear(geiss::paletteCurve(3, 64), 64.f, 0.01f, "curve 3 is x * x / 64");
    checkNear(geiss::paletteCurve(4, 256), 255.f, 0.01f, "curve 4 is a quarter sine up to 255");
    checkNear(geiss::paletteCurve(5, 20), 70.f, 0.01f, "curve 5 is x * 3.5");
    checkNear(geiss::paletteCurve(6, 0), 0.f, 0.01f, "curve 6 starts black");
    check(geiss::paletteCurve(6, 255) < 180.f, "curve 6 stays dark");
    checkNear(geiss::paletteCurve(7, 0), 32.f, 0.01f, "curve 7 starts at a dim glow");
}

void testBuildPalette() {
    geiss::PaletteRecipe monotone;
    monotone.monotone = true;
    monotone.monotoneOrder = 0;
    monotone.lowBand = 7;
    monotone.highBand = 17;
    const geiss::Palette fx = geiss::buildPalette(monotone, true);
    check(entry(fx, 0) == std::array<int, 3>{0, 0, 0}, "a monotone palette starts black");
    // Order 0: red a*a/64, blue a*2, green sqrt(a)*22.6, truncated.
    check(entry(fx, 127) == std::array<int, 3>{252, 254, 254}, "monotone order 0 at 127");
    check(entry(fx, 200) == entry(fx, 127), "a monotone palette holds its 127 value above");
    check(entry(fx, 10)[0] == 1, "the monotone route has no gamma and no coarse band");

    geiss::PaletteRecipe curves;
    curves.curves = {2, 3, 1};
    const geiss::Palette loud = geiss::buildPalette(curves, false);
    // Red is the first curve, blue the second, and green the third, each times 1.1.
    check(entry(loud, 50) == std::array<int, 3>{110, 175, 42}, "curves drive red, blue, and green in the original's order");
    check(entry(loud, 255)[0] == 255, "curves clamp at 255");
    const geiss::Palette quiet = geiss::buildPalette(curves, true);
    check(entry(quiet, 50)[0] == 140, "a palette built in silence uses 0.3 more gamma");

    curves.lowBand = 7;
    curves.highBand = 17;
    const geiss::Palette coarse = geiss::buildPalette(curves, false);
    check(entry(coarse, 10)[0] == 44, "the coarse band doubles its indices");
    check(entry(coarse, 7)[0] == 15 && entry(coarse, 17)[0] == 37, "the band edges stay normal");

    geiss::PaletteRecipe solar;
    solar.curves = {7, 6, 6};
    check(entry(geiss::buildPalette(solar, false), 0)[0] == 35, "a solar curve tints index 0, as in the original");
}

void testBlendPalettes() {
    geiss::Palette black{};
    geiss::Palette white{};
    white.fill(255);
    check(geiss::blendPalettes(black, white, 0.f) == black, "a blend starts at the old palette");
    check(geiss::blendPalettes(black, white, 1.f) == white, "a blend ends at the new palette");
    check(entry(geiss::blendPalettes(black, white, 0.5f), 9)[1] == 128, "a blend is linear");
}

void testRandomPaletteRecipes() {
    constexpr int draws = 60000;
    geiss::Random random{21};
    int monotone = 0;
    int coarse = 0;
    bool bandsInRange = true;
    bool curvesInRange = true;
    bool atMostOneDark = true;
    std::array<int, 8> curveCounts{};
    std::array<int, 4> orders{};
    for (int i = 0; i < draws; ++i) {
        const geiss::PaletteRecipe recipe = geiss::randomPaletteRecipe(random);
        if (recipe.lowBand >= 0) {
            ++coarse;
            bandsInRange = bandsInRange && recipe.lowBand >= 7 && recipe.lowBand <= 12 && recipe.highBand >= 17 &&
                           recipe.highBand <= 22;
        }
        if (recipe.monotone) {
            ++monotone;
            ++orders.at(static_cast<std::size_t>(recipe.monotoneOrder));
            continue;
        }
        int dark = 0;
        for (const int curve : recipe.curves) {
            curvesInRange = curvesInRange && curve >= 1 && curve <= 7;
            dark += curve == 6 ? 1 : 0;
            ++curveCounts.at(static_cast<std::size_t>(std::clamp(curve, 0, 7)));
        }
        atMostOneDark = atMostOneDark && dark <= 1;
    }
    check(monotone > draws / 7 && monotone < draws / 5, "about 1 in 6 palettes is monotone");
    check(coarse > draws / 12 && coarse < draws / 8, "about 1 in 10 palettes has a coarse band");
    check(bandsInRange, "the coarse band runs from 7...12 to 17...22");
    check(curvesInRange, "curves are 1...7");
    check(atMostOneDark, "at most one channel uses the dark curve");
    check(curveCounts[7] > 0 && curveCounts[7] < curveCounts[1] / 3, "the solar curve comes up rarely");
    check(orders[0] > 0 && orders[1] > 0 && orders[2] > 0 && orders[3] > 0, "every monotone order comes up");
}

void testEnginePalettes() {
    // Over 30 s the 20-second timer changes the unlocked engine's palette, but not the locked one's.
    geiss::Engine locked(4);
    geiss::Engine unlocked(4);
    locked.resize(64, 48);
    unlocked.resize(64, 48);
    locked.setLocked(true);
    const geiss::PaletteRecipe held = locked.palette();
    check(unlocked.palette() == held, "the same seed picks the same first palette");
    for (int i = 0; i < 300; ++i) {
        locked.tick(0.1, beatFrame(false, 1.f, 0.f));
        unlocked.tick(0.1, beatFrame(false, 1.f, 0.f));
    }
    check(!(unlocked.palette() == held), "the timer changes an unlocked palette");
    check(locked.palette() == held, "lock holds the palette");

    geiss::Engine next(4);
    next.resize(64, 48);
    const geiss::PaletteRecipe first = next.palette();
    bool changed = false;
    for (int i = 0; i < 10 && !changed; ++i) {
        next.nextVisual();
        changed = !(next.palette() == first);
    }
    check(changed, "Next picks a new palette");

    // A full-brightness line draws at the top of a plain x * 2 palette: 255 * 2 * 1.1, clamped.
    geiss::Engine top(1);
    top.resize(32, 24);
    top.setEffects({});
    geiss::PaletteRecipe plain;
    plain.curves = {2, 2, 2};
    top.setPalette(plain);
    const geiss::FrameView frame = top.tick(1.0 / 30.0, beatFrame(false, 1.f, 0.f));
    int brightest = 0;
    const std::size_t pixels = static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    for (std::size_t i = 0; i < pixels; ++i) {
        brightest = std::max(brightest, static_cast<int>(frame.rgba[i * 4]));
    }
    check(brightest == 255, "a full-brightness line draws at the palette's top");
    check(top.palette() == plain, "setPalette replaces the palette");

    geiss::PaletteRecipe wild;
    wild.curves = {0, 9, 2};
    wild.monotoneOrder = 7;
    top.setPalette(wild);
    check(top.palette().curves == std::array<int, 3>{1, 7, 2}, "setPalette clamps curves to 1...7");
    check(top.palette().monotoneOrder == 3, "setPalette clamps the monotone order to 0...3");
    check(entry(geiss::buildPalette(wild, false), 0)[0] == 0, "an out-of-range curve does not turn white");
}

void testEnginePaletteBlend() {
    const geiss::AudioFrame loud = beatFrame(false, 1.f, 0.f);
    geiss::Engine engine(6);
    engine.resize(32, 24);
    engine.setEffects({});
    const geiss::Palette old = engine.paletteColors();
    engine.nextVisual();
    const geiss::Palette target = geiss::buildPalette(engine.palette(), false);
    check(engine.paletteColors() == old, "a new palette does not jump in");

    engine.tick(0.1, loud);
    const geiss::Palette third = engine.paletteColors();
    const geiss::Palette expected = geiss::blendPalettes(old, target, 1.f / 3.f);
    bool nearThird = !(third == target);
    for (std::size_t i = 0; i < third.size(); ++i) {
        nearThird = nearThird && std::abs(static_cast<int>(third[i]) - static_cast<int>(expected[i])) <= 1;
    }
    check(nearThird, "after 0.1 s the blend is a third of the way");

    engine.setPaused(true);
    engine.tick(0.1, loud);
    check(engine.paletteColors() == third, "pause holds the blend");

    engine.setPaused(false);
    engine.setLocked(true);
    engine.tick(0.1, loud);
    engine.tick(0.1, loud);
    check(engine.paletteColors() == target, "the blend ends at the new palette after 0.3 s, even while locked");
    engine.tick(0.1, loud);
    check(engine.paletteColors() == target, "the blend stays at the new palette");
}

void testPaletteSilentGamma() {
    geiss::PaletteRecipe plain;
    plain.curves = {2, 2, 2};
    const geiss::Palette normal = geiss::buildPalette(plain, false);
    const geiss::Palette boosted = geiss::buildPalette(plain, true);

    geiss::Engine fresh(2);
    fresh.resize(32, 24);
    fresh.setPalette(plain);
    check(fresh.paletteColors() == normal, "the first palette after launch has the normal gamma");

    geiss::AudioFrame quiet = beatFrame(false, 1.f, 0.f);
    quiet.silent = true;
    geiss::Engine engine(2);
    engine.resize(32, 24);
    for (int i = 0; i < 300; ++i) {
        engine.tick(0.1, quiet);
    }
    engine.setPalette(plain);
    check(engine.paletteColors() == normal, "30 s of silence is not yet SoundEmpty");
    for (int i = 0; i < 80; ++i) {
        engine.tick(0.1, quiet);
    }
    engine.setPalette(plain);
    check(engine.paletteColors() == boosted, "after about 37 s of silence a palette gets the silent gamma");
    engine.tick(0.1, beatFrame(false, 1.f, 0.f));
    engine.setPalette(plain);
    check(engine.paletteColors() == normal, "sound ends SoundEmpty at once");
}

geiss::AudioFrame bassFrame(bool beatMode, float bass, float average) {
    geiss::AudioFrame audio;
    audio.beatMode = beatMode;
    audio.bass = bass;
    audio.bassAverage = average;
    return audio;
}

// One loud hit after `quietFrames` quiet frames at 30 fps. Each hit is louder than the last
// so it always beats the peak, which sits 5% above the previous hit.
void hit(geiss::SlideShift& shift, int quietFrames, float& loud, int width = 640) {
    for (int i = 0; i < quietFrames; ++i) {
        shift.update(1.0 / 30.0, bassFrame(true, 0.f, 0.1f), width);
    }
    loud *= 1.06f;
    shift.update(1.0 / 30.0, bassFrame(true, loud, 0.1f), width);
}

void testSlideShiftPicks() {
    constexpr int draws = 30000;
    geiss::Random random{13};
    int allowed = 0;
    for (int i = 0; i < draws; ++i) {
        allowed += geiss::SlideShift::rollAllowed(random) ? 1 : 0;
    }
    check(allowed > draws * 30 / 100 && allowed < draws * 36 / 100, "about a third of looks allow the slide shift");

    geiss::SlideShift off(1);
    off.startLook(false);
    float loud = 1.f;
    for (int i = 0; i < 20; ++i) {
        hit(off, 4, loud);
    }
    check(off.count() == 0 && off.x() == 0.f && off.y() == 0.f, "a look without the shift never drifts");

    geiss::SlideShift calm(1);
    calm.startLook(true);
    for (int i = 0; i < 20; ++i) {
        calm.update(1.0 / 30.0, bassFrame(false, static_cast<float>(i), 0.1f), 640);
    }
    check(calm.count() == 0, "the shift waits for beat mode");
}

void testSlideShiftOnPeaks() {
    geiss::SlideShift shift(3);
    shift.startLook(true);
    float loud = 1.f;
    hit(shift, 0, loud);
    check(shift.count() == 0, "the first peak only sets the level to beat");
    hit(shift, 3, loud);
    check(shift.count() == 1, "a peak after three quiet frames shifts");
    hit(shift, 2, loud);
    check(shift.count() == 1, "a peak within two quiet frames does not shift");
    hit(shift, 3, loud);
    check(shift.count() == 2, "the next spaced peak shifts again");

    std::set<int> xs;
    std::set<int> ys;
    bool valid = true;
    bool alternates = true;
    for (int i = 0; i < 400; ++i) {
        const float before = shift.x();
        hit(shift, 3, loud);
        const float x = shift.x();
        const float y = shift.y();
        valid = valid && x == std::round(x) && std::fabs(x) <= 2.f && (y == -1.f || y == 0.f || y == 1.f);
        alternates = alternates && (before > 0.f ? x <= 0.f : x >= 0.f);
        xs.insert(static_cast<int>(x));
        ys.insert(static_cast<int>(y));
    }
    check(shift.count() == 402, "every spaced, louder peak shifts");
    check(valid, "a shift is 0 to 2 pixels sideways and one row at most at 640 wide");
    check(alternates, "the sideways direction alternates");
    check(xs.size() == 5 && ys.size() == 3, "every offset comes up");

    geiss::SlideShift wide(3);
    wide.startLook(true);
    float wideLoud = 1.f;
    hit(wide, 0, wideLoud, 1280);
    bool doubled = true;
    for (int i = 0; i < 40; ++i) {
        hit(wide, 3, wideLoud, 1280);
        doubled = doubled && std::fmod(wide.x(), 2.f) == 0.f && std::fmod(wide.y(), 2.f) == 0.f;
    }
    check(doubled, "the shift scales with the width");

    const float heldX = shift.x();
    const float heldY = shift.y();
    for (int i = 0; i < 30; ++i) {
        shift.update(1.0 / 30.0, bassFrame(false, 0.f, 0.f), 640);
    }
    check(shift.x() == heldX && shift.y() == heldY, "the drift holds after beat mode ends, as in the original");
    shift.startLook(true);
    check(shift.x() == heldX && shift.y() == heldY, "a look that allows the shift keeps the drift");
    shift.startLook(false);
    check(shift.x() == 0.f && shift.y() == 0.f, "a look without the shift stops the drift");
}

void testPeakRelaxes() {
    // After a very loud hit, a steady beat at a quarter of it shifts again once the peak
    // has relaxed toward 1.43 times the average.
    geiss::SlideShift shift(5);
    shift.startLook(true);
    shift.update(1.0 / 30.0, bassFrame(true, 4.f, 0.5f), 640);
    int frames = 0;
    while (shift.count() == 0 && frames < 300) {
        shift.update(1.0 / 30.0, bassFrame(true, 0.f, 0.5f), 640);
        shift.update(1.0 / 30.0, bassFrame(true, 0.f, 0.5f), 640);
        shift.update(1.0 / 30.0, bassFrame(true, 0.f, 0.5f), 640);
        shift.update(1.0 / 30.0, bassFrame(true, 1.f, 0.5f), 640);
        frames += 4;
    }
    // 4.2 relaxes toward 0.715 at 0.975 per frame and falls below 1 after about 99 frames.
    check(frames >= 96 && frames <= 104, "the peak relaxes toward 1.43 times the bass average");

    // The same relaxation at 60 fps takes the same time.
    geiss::SlideShift fast(5);
    fast.startLook(true);
    fast.update(1.0 / 60.0, bassFrame(true, 4.f, 0.5f), 640);
    int fastFrames = 0;
    while (fast.count() == 0 && fastFrames < 600) {
        for (int i = 0; i < 7; ++i) {
            fast.update(1.0 / 60.0, bassFrame(true, 0.f, 0.5f), 640);
        }
        fast.update(1.0 / 60.0, bassFrame(true, 1.f, 0.5f), 640);
        fastFrames += 8;
    }
    check(std::abs(fastFrames / 2 - frames) <= 4, "the peak relaxes by time, not frames");
}

void testWarpShift() {
    constexpr int width = 16;
    constexpr int height = 12;
    std::vector<geiss::SourcePoint> identity;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            identity.push_back({static_cast<float>(x), static_cast<float>(y)});
        }
    }
    std::vector<std::uint8_t> src(std::size_t{width} * height, 0);
    std::vector<std::uint8_t> dst(src.size(), 0);
    src[std::size_t{5} * width + 5] = 200;
    geiss::warpIndices(src.data(), dst.data(), width, height, identity.data(), 1.f, 1.f, 1.f, 0.f);
    check(dst[std::size_t{5} * width + 4] == 200, "a positive sideways shift slides the picture left");
    geiss::warpIndices(src.data(), dst.data(), width, height, identity.data(), 1.f, 1.f, 0.f, -1.f);
    check(dst[std::size_t{6} * width + 5] == 200, "a negative vertical shift slides the picture down");
}

void testEngineSlideShift() {
    geiss::Engine engine(8);
    engine.resize(64, 48);
    int allowed = 0;
    for (int i = 0; i < 600; ++i) {
        engine.nextVisual();
        allowed += engine.slideShift().allowed() ? 1 : 0;
    }
    check(allowed > 150 && allowed < 250, "Next rolls the slide shift for each look");

    engine.setSlideShiftAllowed(true);
    engine.setLocked(true);
    float loud = 1.f;
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 3; ++j) {
            engine.tick(1.0 / 30.0, bassFrame(true, 0.f, 0.1f));
        }
        loud *= 1.06f;
        engine.tick(1.0 / 30.0, bassFrame(true, loud, 0.1f));
    }
    check(engine.slideShift().allowed() && engine.slideShift().count() == 10, "a locked look keeps shifting on peaks");
}

// Mean of a uniform 100 buffer after one second of the 255/256 darkening at `fps`,
// through an identity map.
float fadeAfterOneSecond(int fps) {
    constexpr int width = 64;
    constexpr int height = 32;
    std::vector<geiss::SourcePoint> identity;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            identity.push_back({static_cast<float>(x), static_cast<float>(y)});
        }
    }
    std::vector<std::uint8_t> a(std::size_t{width} * height, 100);
    std::vector<std::uint8_t> b(a.size(), 0);
    const auto decay = static_cast<float>(std::pow(255.0 / 256.0, 30.0 / fps));
    for (int i = 0; i < fps; ++i) {
        geiss::warpIndices(a.data(), b.data(), width, height, identity.data(), 1.f, decay, 0.f, 0.f,
                           static_cast<std::uint32_t>(i + 1));
        a.swap(b);
    }
    double sum = 0;
    for (const std::uint8_t value : a) {
        sum += value;
    }
    return static_cast<float>(sum / static_cast<double>(a.size()));
}

void testFadeMatchesAtAnyFrameRate() {
    const float expected = 100.f * static_cast<float>(std::pow(255.0 / 256.0, 30.0));
    checkNear(fadeAfterOneSecond(30), expected, 1.f, "a second of darkening at 30 fps");
    checkNear(fadeAfterOneSecond(60), expected, 1.f, "a second of darkening at 60 fps matches 30 fps");
    checkNear(fadeAfterOneSecond(120), expected, 1.f, "a second of darkening at 120 fps matches 30 fps");

    double total = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const float t = geiss::ditherThreshold(x, y, 7);
            check(t >= 0.f && t < 1.f, "a rounding threshold is in 0...1");
            total += t;
        }
    }
    checkNear(static_cast<float>(total / 4096.0), 0.5f, 0.02f, "rounding thresholds average one half");
    check(geiss::ditherThreshold(3, 4, 1) != geiss::ditherThreshold(3, 4, 2), "the rounding pattern changes with the seed");
}

void testBassAverage() {
    geiss::BeatDetector detector;
    geiss::BeatState state;
    for (int i = 0; i < 60; ++i) {
        state = detector.update(0.3f, 1.0 / 30.0);
    }
    checkNear(state.bassAverage, 0.3f, 0.01f, "the detector reports the average smoothed bass");
}

geiss::MapParameters centeredMap(int mode) {
    geiss::Random random{7};
    geiss::MapParameters map = geiss::randomMapParameters(mode, 64, 64, random);
    map.centerXNorm = 0.5f;
    map.centerYNorm = 0.5f;
    map.turn = 0;
    map.turn2 = 0;
    return map;
}

// The scale a source point implies for an unrotated map centered at (32, 32).
float scaleAt(int x, int y, const geiss::MapParameters& map) {
    const geiss::SourcePoint source = geiss::mapSource(x, y, 64, 64, map);
    const float dx = static_cast<float>(x) - 32.f;
    const float dy = static_cast<float>(y) - 32.f;
    return std::fabs(dx) > std::fabs(dy) ? (source.x - 32.f) / dx : (source.y - 32.f) / dy;
}

void testEveryModeBuildsAFiniteMap() {
    for (int mode = geiss::kFirstMapMode; mode <= geiss::kLastMapMode; ++mode) {
        geiss::Random random{static_cast<std::uint32_t>(mode * 31 + 1)};
        const geiss::MapParameters map = geiss::randomMapParameters(mode, 96, 54, random);
        check(map.mode == mode, "random parameters keep the mode");
        std::vector<geiss::SourcePoint> sources;
        geiss::buildMap(sources, 96, 54, map);
        bool finite = sources.size() == static_cast<std::size_t>(96 * 54);
        for (const geiss::SourcePoint& point : sources) {
            finite = finite && std::isfinite(point.x) && std::isfinite(point.y) && std::fabs(point.x) < 960.f &&
                     std::fabs(point.y) < 540.f;
        }
        if (!finite) {
            std::cerr << "mode " << mode << ": ";
        }
        check(finite, "every mode builds a finite map near the frame");
    }
}

void testModeDampingAndDecay() {
    for (int mode = geiss::kFirstMapMode; mode <= geiss::kLastMapMode; ++mode) {
        geiss::Random random{3};
        const geiss::MapParameters map = geiss::randomMapParameters(mode, 64, 48, random);
        const bool dampened = mode <= 16 && mode != 3 && mode != 6;
        checkNear(map.damping, dampened ? 0.5f : 1.f, 0.f, "mode_motion_dampened halves modes 1-16 but 3 and 6");
        checkNear(map.decay, mode == 12 ? 0.98f : 1.f, 0.f, "only mode 12 fades faster");
    }
}

void testNewModeShapes() {
    // 18 is 17 turned on its side.
    const geiss::MapParameters horizontal = centeredMap(17);
    const geiss::MapParameters vertical = centeredMap(18);
    const geiss::SourcePoint across = geiss::mapSource(40, 20, 64, 64, horizontal);
    const geiss::SourcePoint down = geiss::mapSource(20, 40, 64, 64, vertical);
    checkNear(across.x, down.y, 0.001f, "mode 18 is mode 17 transposed (x)");
    checkNear(across.y, down.x, 0.001f, "mode 18 is mode 17 transposed (y)");

    // 21 is diced: one scale per tenth of the half-width in each direction.
    const geiss::MapParameters diced = centeredMap(21);
    checkNear(scaleAt(33, 32, diced), scaleAt(34, 32, diced), 0.0001f, "mode 21 holds its scale within a cell");
    check(scaleAt(60, 32, diced) < scaleAt(34, 32, diced) - 0.1f, "mode 21 steps its scale between cells");

    // 22 and 23 are rings: 22 steps outward, 23 repeats every four rings.
    const geiss::MapParameters rings = centeredMap(22);
    check(scaleAt(36, 32, rings) > scaleAt(60, 32, rings), "mode 22 steps down at each ring");
    const geiss::MapParameters fastRings = centeredMap(23);
    checkNear(scaleAt(32 + 1, 32, fastRings), scaleAt(32 + 1 + 13, 32, fastRings), 0.0001f,
              "mode 23 repeats its rings every four");

    // 20 does not break above the frame, where the original took a negative root.
    const geiss::MapParameters terra = centeredMap(20);
    const geiss::SourcePoint top = geiss::mapSource(10, -40, 64, 64, terra);
    check(std::isfinite(top.x) && std::isfinite(top.y), "mode 20 stays finite above the frame");

    // 15 has f1 petals: the same scale one petal around.
    geiss::MapParameters petals = centeredMap(15);
    petals.f1 = 4;
    const geiss::SourcePoint east = geiss::mapSource(52, 32, 64, 64, petals);
    const geiss::SourcePoint south = geiss::mapSource(32, 52, 64, 64, petals);
    checkNear(east.x - 32.f, south.y - 32.f, 0.001f, "mode 15 repeats its scale each petal");

    // 16, the crystal ball, flips far pixels through the center.
    const geiss::MapParameters ball = centeredMap(16);
    check(scaleAt(36, 32, ball) > 0.9f, "mode 16 barely moves near the center");
    check(geiss::mapSource(0, 32, 64, 64, ball).x > 32.f, "mode 16 reads far pixels from the other side");

    // 12 splits sideways: the centerline reads itself, the sides read near the center.
    const geiss::MapParameters splitter = centeredMap(12);
    checkNear(geiss::mapSource(32, 10, 64, 64, splitter).x, 32.f, 0.001f, "mode 12 holds the centerline");
    const geiss::SourcePoint side = geiss::mapSource(60, 10, 64, 64, splitter);
    check(side.x > 32.f && side.x < 45.f, "mode 12 streaks the sides out of the middle");
    checkNear(side.y, 10.f, 0.001f, "mode 12 keeps rows");

    // 10 scrolls: rows read from 4% further down.
    const geiss::MapParameters scroll = centeredMap(10);
    checkNear(geiss::mapSource(32, 50, 64, 64, scroll).y, 52.f, 0.001f, "mode 10 reads rows from y * 1.04");

    // 6 moves each pixel by its pushes, about 1.9 pixels at 640 wide.
    geiss::Random vortexRandom{11};
    const geiss::MapParameters vortex = geiss::randomMapParameters(6, 640, 480, vortexRandom);
    const geiss::SourcePoint moved = geiss::mapSource(300, 200, 640, 480, vortex);
    const float distance = std::hypot(moved.x - 300.f, moved.y - 200.f);
    check(distance > 0.05f && distance < 6.f, "mode 6 moves a pixel a few pixels at 640 wide");
    geiss::Random wideRandom{11};
    const geiss::MapParameters wideVortex = geiss::randomMapParameters(6, 1280, 960, wideRandom);
    const geiss::SourcePoint wideMoved = geiss::mapSource(600, 400, 1280, 960, wideVortex);
    checkNear(std::hypot(wideMoved.x - 600.f, wideMoved.y - 400.f), distance * 2.f, 0.05f,
              "mode 6 keeps its look at twice the size");

    // 11 dithers two rotations by pixel parity.
    geiss::Random swirlRandom{5};
    const geiss::MapParameters dither = geiss::randomMapParameters(11, 64, 64, swirlRandom);
    check(std::fabs(dither.turn - dither.turn2) > 0.01f && dither.scale != dither.scale2, "mode 11 has two transforms");

    // 7 is fuzzy: neighbors jitter apart, within 0.05 of scale.
    const geiss::MapParameters fuzzy = centeredMap(7);
    float smallest = 1;
    float largest = 0;
    for (int x = 50; x < 60; ++x) {
        const float scale = scaleAt(x, 32, fuzzy) - (fuzzy.f1 - std::fabs(static_cast<float>(x) - 32.f) * 10.f * fuzzy.f2);
        smallest = std::min(smallest, scale);
        largest = std::max(largest, scale);
    }
    check(smallest >= -0.0001f && largest <= 0.0496f && largest > smallest, "mode 7 jitters each pixel by up to 0.0495");

    // 3 zooms in less toward the bottom.
    const geiss::MapParameters landing = centeredMap(3);
    check(scaleAt(32, 10, landing) > scaleAt(32, 54, landing), "mode 3 slows its zoom down the frame");
}

void testLookPicking() {
    check(!geiss::lookAllowed(6, 5), "mode 6 never draws waveform 5");
    check(!geiss::lookAllowed(12, 4) && !geiss::lookAllowed(12, 6), "mode 12 never draws waveforms 4 or 6");
    check(!geiss::lookAllowed(14, 3) && !geiss::lookAllowed(14, 4), "mode 14 never draws waveforms 3 or 4");
    check(!geiss::lookAllowed(8, 6) && !geiss::lookAllowed(23, 6) && !geiss::lookAllowed(24, 6),
          "modes 8, 23, and 24 never draw the oscilloscope");
    check(geiss::lookAllowed(25, 1) && !geiss::lookAllowed(26, 1) && !geiss::lookAllowed(1, 7), "unknown numbers are not looks");

    constexpr int draws = 50000;
    std::vector<int> modes(26, 0);
    std::vector<int> waves(7, 0);
    geiss::Random random{99};
    bool allowed = true;
    for (int i = 0; i < draws; ++i) {
        const geiss::Look look = geiss::randomLook(random);
        allowed = allowed && geiss::lookAllowed(look.mode, look.waveform);
        ++modes[static_cast<std::size_t>(look.mode)];
        ++waves[static_cast<std::size_t>(look.waveform)];
    }
    check(allowed, "a random look is always an allowed pair");
    bool everyMode = true;
    for (int mode = 1; mode <= 25; ++mode) {
        everyMode = everyMode && modes[static_cast<std::size_t>(mode)] > 0;
    }
    check(everyMode, "every map mode comes up");
    check(modes[5] > modes[1] * 3 / 2 && modes[7] > modes[1] * 3 / 2, "modes 5 and 7 come up more often");
    check(waves[6] < waves[1] && waves[5] > 0 && waves[3] > 0, "waveform 6 comes up less often");
}

// A fresh engine moved to `waveform` by Next, ticked once with a silent-looking flat wave.
// A plain x * 2 palette, whose index 0 is black, and no slide shift, so a test sees
// only what it draws.
void pinLook(geiss::Engine& engine) {
    geiss::PaletteRecipe plain;
    plain.curves = {2, 2, 2};
    engine.setPalette(plain);
    engine.setSlideShiftAllowed(false);
}

geiss::Engine engineDrawing(int waveform, int width, int height) {
    geiss::Engine engine(17);
    engine.resize(width, height);
    for (int i = 0; i < 500 && engine.waveform() != waveform; ++i) {
        engine.nextVisual();
    }
    check(engine.waveform() == waveform, "Next reaches every waveform");
    engine.setEffects({});
    pinLook(engine);
    return engine;
}

std::vector<std::pair<int, int>> litPixels(const geiss::FrameView& frame) {
    std::vector<std::pair<int, int>> lit;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x)) * 4;
            if (frame.rgba[i] + frame.rgba[i + 1] + frame.rgba[i + 2] > 0) {
                lit.emplace_back(x, y);
            }
        }
    }
    return lit;
}

geiss::AudioFrame flatWave(int columns) {
    geiss::AudioFrame audio = beatFrame(false, 1.f, 0.f);
    audio.samples.assign(static_cast<std::size_t>(columns), 0.f);
    audio.left = audio.samples;
    audio.right = audio.samples;
    return audio;
}

void testNewWaveforms() {
    constexpr int width = 160;
    constexpr int height = 90;

    geiss::Engine vertical = engineDrawing(3, width, height);
    const float centerX = vertical.map().centerXNorm * width;
    const auto verticalLit = litPixels(vertical.tick(1.0 / 30.0, flatWave(width)));
    // The smoothed position can land a hair under a whole-pixel center, so allow one column.
    bool onCenter = !verticalLit.empty() && std::fabs(static_cast<float>(verticalLit.front().first) - centerX) <= 1.f;
    for (const auto& [x, y] : verticalLit) {
        onCenter = onCenter && x == verticalLit.front().first;
    }
    check(onCenter && verticalLit.size() == static_cast<std::size_t>(height), "waveform 3 is one vertical line through the center");

    geiss::Engine diagonal = engineDrawing(4, width, height);
    const auto diagonalLit = litPixels(diagonal.tick(1.0 / 30.0, flatWave(width)));
    bool onDiagonals = !diagonalLit.empty();
    for (const auto& [x, y] : diagonalLit) {
        onDiagonals = onDiagonals && (x == y || x == y + width - height);
    }
    check(onDiagonals && diagonalLit.size() == static_cast<std::size_t>(height) * 2, "waveform 4 is two diagonals");

    geiss::Engine circle = engineDrawing(5, width, height);
    const float cx = circle.map().centerXNorm * width;
    const float cy = circle.map().centerYNorm * height;
    const auto circleLit = litPixels(circle.tick(1.0 / 30.0, flatWave(width)));
    const float radius = 60.f * width / 640.f;
    bool onRing = circleLit.size() > 20;
    for (const auto& [x, y] : circleLit) {
        onRing = onRing && std::fabs(std::hypot(static_cast<float>(x) - cx, static_cast<float>(y) - cy) - radius) < 2.f;
    }
    check(onRing, "waveform 5 is a circle of 60 pixels at 640 wide around the center");

    // The first 50 points blend toward the points after 314, so a rising wave closes
    // the circle instead of jumping back to its starting radius.
    constexpr int ringWidth = 320;
    constexpr int ringHeight = 240;
    geiss::Engine closing = engineDrawing(5, ringWidth, ringHeight);
    const float closeX = closing.map().centerXNorm * ringWidth;
    const float closeY = closing.map().centerYNorm * ringHeight;
    geiss::AudioFrame ramp = flatWave(ringWidth);
    for (std::size_t i = 0; i < ramp.samples.size(); ++i) {
        ramp.samples[i] = 0.4f * static_cast<float>(i) / static_cast<float>(ramp.samples.size() - 1);
    }
    float startRadius = 0;
    int startPoints = 0;
    for (const auto& [x, y] : litPixels(closing.tick(1.0 / 30.0, ramp))) {
        const float angle = std::atan2(static_cast<float>(y) - closeY, static_cast<float>(x) - closeX);
        if (angle > 0.1f && angle < 0.2f) {
            startRadius += std::hypot(static_cast<float>(x) - closeX, static_cast<float>(y) - closeY);
            ++startPoints;
        }
    }
    // Unblended, these points would sit near the base radius of 30. Blended, they sit near
    // the end of the ramp, about 50.
    check(startPoints > 0 && startRadius / static_cast<float>(startPoints) > 44.f,
          "waveform 5 blends its start toward its end so the circle closes");
}

// A fresh engine moved by Next to `mode` with waveform 1.
geiss::Engine engineShowing(int mode, int width, int height) {
    geiss::Engine engine(23);
    engine.resize(width, height);
    for (int i = 0; i < 5000 && (engine.mapMode() != mode || engine.waveform() != 1); ++i) {
        engine.nextVisual();
    }
    check(engine.mapMode() == mode && engine.waveform() == 1, "Next reaches the requested map with waveform 1");
    engine.setEffects({});
    pinLook(engine);
    return engine;
}

void testLineSitsLowerOverScroll() {
    constexpr int width = 160;
    constexpr int height = 90;
    geiss::Engine scroll = engineShowing(10, width, height);
    const auto lit = litPixels(scroll.tick(1.0 / 30.0, flatWave(width)));
    bool low = lit.size() == static_cast<std::size_t>(width);
    for (const auto& [x, y] : lit) {
        low = low && y == static_cast<int>(height * 0.75f);
    }
    check(low, "waveform 1 sits at three quarters of the height over mode 10");

    geiss::Engine zoom = engineShowing(25, width, height);
    const auto middle = litPixels(zoom.tick(1.0 / 30.0, flatWave(width)));
    check(!middle.empty() && middle.front().second == height / 2, "waveform 1 sits at the middle over other maps");
}

void testLowModeShapes() {
    // r640 is 80 at (40, 32) on a 64-wide frame centered at (32, 32).
    constexpr float r = 80.f;

    const geiss::MapParameters constant = centeredMap(1);
    checkNear(scaleAt(40, 32, constant), constant.scale, 0.0001f, "mode 1 uses its constant scale");
    checkNear(scaleAt(41, 32, constant), constant.scale, 0.0001f, "mode 1 dithers to the same scale");

    const geiss::MapParameters outward = centeredMap(2);
    check(outward.scale >= 1.f, "mode 2 zooms outward");
    checkNear(scaleAt(40, 32, outward), outward.scale, 0.0001f, "mode 2 uses its constant scale");

    const geiss::MapParameters sphere = centeredMap(4);
    checkNear(scaleAt(40, 32, sphere), 0.9f + r * 0.0025f * 0.14f, 0.0001f, "mode 4 grows its scale with the radius");

    const geiss::MapParameters perspective = centeredMap(5);
    checkNear(scaleAt(40, 32, perspective), perspective.f2 - perspective.f1 * (r / 200.f * 1.7f), 0.0001f,
              "mode 5 is super-perspective");

    const geiss::MapParameters ripples = centeredMap(8);
    checkNear(scaleAt(40, 32, ripples), 0.85f + 0.1f * std::sin(std::sqrt(r) * ripples.f1), 0.0001f,
              "mode 8 ripples with the root of the radius");

    const geiss::MapParameters hole = centeredMap(13);
    checkNear(scaleAt(40, 32, hole), (1.04f - r * std::sqrt(r) * 0.00025f * 0.14f - 1.f) * hole.f1 + 1.f, 0.0001f,
              "mode 13 is a black hole");

    const geiss::MapParameters split = centeredMap(14);
    checkNear(scaleAt(32, 40, split), 0.9f + 0.2f * std::cos(8.f * 12.f / 64.f), 0.0001f,
              "mode 14 follows the cosine of the height");
}

void testDitherParity() {
    // Pixels where x and y have the same parity use the first transform; the others use scale2.
    const geiss::MapParameters petals = centeredMap(9);
    checkNear(scaleAt(40, 32, petals), petals.f1 - 80.f * petals.f2, 0.0001f, "mode 9 even pixels use f1 - r*f2");
    checkNear(scaleAt(41, 32, petals), petals.scale2, 0.0001f, "mode 9 odd pixels use scale2");
    checkNear(scaleAt(41, 33, petals), petals.f1 - std::hypot(9.f, 1.f) * 10.f * petals.f2, 0.0001f,
              "mode 9 dithers by the parity of x and y together");

    const geiss::MapParameters twin = centeredMap(11);
    checkNear(scaleAt(40, 32, twin), twin.scale, 0.0001f, "mode 11 even pixels use scale");
    checkNear(scaleAt(41, 32, twin), twin.scale2, 0.0001f, "mode 11 odd pixels use scale2");
}

void testTallFrameKeepsScalesPositive() {
    // On a frame much taller than wide, the corners sit far beyond the 640x480 radii the
    // original was tuned for. Modes 7 and 9 must not flip them through the center.
    constexpr int width = 64;
    constexpr int height = 200;
    for (const int mode : {7, 9}) {
        geiss::MapParameters map = centeredMap(mode);
        map.f1 = mode == 9 ? 0.98f : 0.92f;
        map.f2 = mode == 9 ? 0.0021f : 0.0011f;
        bool outward = true;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const geiss::SourcePoint source = geiss::mapSource(x, y, width, height, map);
                const float dx = static_cast<float>(x) - 32.f;
                const float dy = static_cast<float>(y) - 100.f;
                outward = outward && (source.x - 32.f) * dx + (source.y - 100.f) * dy >= -0.0001f;
            }
        }
        check(outward, "modes 7 and 9 never read from the other side on a tall frame");
    }
}

void testRandomLeavesZero() {
    geiss::Random random{0};
    const std::uint32_t first = random.next();
    check(first != 0 && random.next() != first, "a zero random state still moves");
    geiss::Random looks{0};
    const geiss::Look a = geiss::randomLook(looks);
    bool varied = false;
    for (int i = 0; i < 20; ++i) {
        const geiss::Look b = geiss::randomLook(looks);
        varied = varied || b.mode != a.mode || b.waveform != a.waveform;
    }
    check(varied, "looks from a zero random state vary");
}

void testLookWaitsForBigBeat() {
    geiss::Engine plain(1);
    plain.resize(16, 12);
    const double withoutBeat = secondsUntilLookChanges(plain, beatFrame(false, 1.f, 0.f), 30);
    check(withoutBeat > 19.9 && withoutBeat < 20.2, "without a beat the look changes at 20 s");

    geiss::Engine waiting(1);
    waiting.resize(16, 12);
    const double noBigBeat = secondsUntilLookChanges(waiting, beatFrame(true, 1.f, 0.5f), 30);
    check(noBigBeat > 24.9 && noBigBeat < 25.2, "with no big beat the look changes when the 5 s wait ends");

    geiss::Engine hit(1);
    hit.resize(16, 12);
    const double bigBeat = secondsUntilLookChanges(hit, beatFrame(true, 1.f, 1.2f), 30);
    check(bigBeat > 19.9 && bigBeat < 20.2, "a big beat changes the look as soon as it is due");

    geiss::Engine steady(1);
    steady.resize(16, 12);
    const double steadyBeat = secondsUntilLookChanges(steady, beatFrame(true, 1.f, 1.05f), 30);
    check(steadyBeat > 21.0 && steadyBeat < 22.0, "a steady beat lands the change after the threshold relaxes");

    geiss::Engine locked(1);
    locked.resize(16, 12);
    locked.setLocked(true);
    check(secondsUntilLookChanges(locked, beatFrame(true, 1.f, 2.f), 30) < 0, "lock holds the look through big beats");
}


void testModeEffectsTable() {
    using geiss::Effect;
    const geiss::ModeEffects one = geiss::modeEffects(1);
    const auto at = [](const geiss::ModeEffects& info, Effect effect) { return info.weights[static_cast<std::size_t>(effect)]; };
    check(at(one, Effect::Chasers) == 170 && at(one, Effect::Bar) == 370 && at(one, Effect::Dots) == 230 &&
              at(one, Effect::Solar) == 680 && at(one, Effect::Grid) == 12 && at(one, Effect::Nuclide) == 220 &&
              at(one, Effect::Shade) == 550,
          "mode 1 takes the truecolor adjustment of its weights");
    // 170 * 1.3f is a hair under 221 in float, and the original truncated it.
    check(one.solarMax == 400, "solar strength comes from the 8-bit table");
    check(at(geiss::modeEffects(8), Effect::Shade) == 210, "mode 8 keeps its chained shade weight of 60");
    const geiss::ModeEffects fifteen = geiss::modeEffects(15);
    check(fifteen.minEffects == 0 && fifteen.maxEffects == 1 && at(fifteen, Effect::Chasers) == 0, "mode 15 allows one effect");
    checkNear(geiss::modeEffects(12).centerDwindle, 0.915f, 0.0001f, "mode 12 darkens its centerline");
    checkNear(geiss::modeEffects(21).centerDwindle, 0.98f, 0.0001f, "modes 20 to 23 darken the center");
    checkNear(geiss::modeEffects(25).centerDwindle, 1.f, 0.0001f, "mode 25 leaves the center alone");
    check(geiss::modeEffects(25).maxEffects == 3, "modes 17 and up allow three effects");
    bool capped = true;
    for (int mode = geiss::kFirstMapMode; mode <= geiss::kLastMapMode; ++mode) {
        for (const int weight : geiss::modeEffects(mode).weights) {
            capped = capped && weight >= 0 && weight <= 900;
        }
    }
    check(capped, "every weight stays within 0...900");
}

void testRandomEffectsRespectLimits() {
    using geiss::Effect;
    geiss::Random random{11};
    bool withinMax = true;
    bool withinMin = true;
    bool noBarWithGrid = true;
    bool hideNeedsNuclide = true;
    bool zeroNeverComes = true;
    bool chaserCount = true;
    for (int mode = geiss::kFirstMapMode; mode <= geiss::kLastMapMode; ++mode) {
        const geiss::ModeEffects info = geiss::modeEffects(mode);
        for (int i = 0; i < 400; ++i) {
            const bool sound = i % 2 == 0;
            const geiss::EffectSet effects = geiss::randomEffects(mode, sound, random);
            const int count = effects.count();
            withinMax = withinMax && count <= info.maxEffects;
            // A grid can turn the bar off after the minimum was met.
            if (!sound) {
                withinMin = withinMin && count >= info.minEffects - 1;
            }
            noBarWithGrid = noBarWithGrid && !(effects.has(Effect::Grid) && effects.has(Effect::Bar));
            hideNeedsNuclide = hideNeedsNuclide && (!effects.hideWaveform || (effects.has(Effect::Nuclide) && mode != 10));
            for (std::size_t e = 0; e < effects.on.size(); ++e) {
                zeroNeverComes = zeroNeverComes && !(effects.on[e] && info.weights[e] == 0);
            }
            chaserCount = chaserCount && (effects.chasers == 1 || effects.chasers == 2);
        }
    }
    check(withinMax, "a look never has more effects than its mode allows");
    check(withinMin, "without sound a look meets its mode's minimum");
    check(noBarWithGrid, "a grid turns the bar off");
    check(hideNeedsNuclide, "only the nuclide hides the waveform, never over mode 10");
    check(zeroNeverComes, "an effect with weight 0 never comes up");
    check(chaserCount, "chasers come as one or two");

    int nuclideLooks = 0;
    int hidden = 0;
    int silentCount = 0;
    int soundCount = 0;
    for (int i = 0; i < 4000; ++i) {
        const geiss::EffectSet effects = geiss::randomEffects(7, false, random);
        if (effects.has(Effect::Nuclide)) {
            ++nuclideLooks;
            hidden += effects.hideWaveform ? 1 : 0;
        }
        silentCount += geiss::randomEffects(2, false, random).count();
        soundCount += geiss::randomEffects(2, true, random).count();
    }
    const float hiddenShare = static_cast<float>(hidden) / static_cast<float>(nuclideLooks);
    check(nuclideLooks > 1500, "mode 7 favors the nuclide");
    checkNear(hiddenShare, 4.f / 7.f, 0.05f, "the nuclide hides the waveform 4 times in 7");
    check(soundCount < silentCount, "sound lowers every weight");
}

using Pixels = std::vector<std::uint8_t>;

int litCount(const Pixels& pixels) {
    return static_cast<int>(std::count_if(pixels.begin(), pixels.end(), [](std::uint8_t p) { return p > 0; }));
}

// The farthest lit pixel from (cx, cy).
float litReach(const Pixels& pixels, int width, float cx, float cy) {
    float reach = 0;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        if (pixels[i] > 0) {
            const int column = static_cast<int>(i) % width;
            const int row = static_cast<int>(i) / width;
            const auto x = static_cast<float>(column);
            const auto y = static_cast<float>(row);
            reach = std::max(reach, std::hypot(x - cx, y - cy));
        }
    }
    return reach;
}

geiss::EffectSet only(geiss::Effect effect) {
    geiss::EffectSet effects;
    effects.set(effect, true);
    return effects;
}

geiss::AudioFrame quietFrame() {
    geiss::AudioFrame audio;
    audio.silent = true;
    return audio;
}

// Runs `frames` reference frames of one effect set on a blank buffer, in ticks of `perFrame`.
Pixels runEffects(const geiss::EffectSet& effects, int mode, int width, int height, int frames, int ticksPerFrame,
                  const geiss::AudioFrame& audio, std::uint32_t seed = 3) {
    Pixels pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    geiss::EffectLayer layer(seed);
    layer.startLook(effects);
    for (int i = 0; i < frames * ticksPerFrame; ++i) {
        layer.draw(pixels.data(), width, height, effects, mode, static_cast<float>(width) / 2.f,
                   static_cast<float>(height) / 2.f, 1.0 / ticksPerFrame, audio);
    }
    return pixels;
}

void testEachEffectDraws() {
    using geiss::Effect;
    constexpr int width = 320;
    constexpr int height = 240;
    constexpr float cx = width / 2.f;
    constexpr float cy = height / 2.f;
    constexpr float scale = width / 640.f;
    const geiss::AudioFrame sound = beatFrame(false, 1.f, 0.f);

    const Pixels shade = runEffects(only(Effect::Shade), 25, width, height, 30, 1, sound);
    check(litCount(shade) > 0 && litReach(shade, width, cx, cy) < 9.6f * scale + 30.f, "shade bobs glow near the center");
    check(*std::max_element(shade.begin(), shade.end()) <= 251, "shade bobs stop adding at 250");

    const Pixels chasers = runEffects(only(Effect::Chasers), 25, width, height, 30, 1, sound);
    check(litCount(chasers) > 100, "chasers trace a path");
    check(litReach(chasers, width, cx, cy) <= (139.f + 109.f) * scale + 2.f, "chasers stay on their curves");

    geiss::EffectSet twoChasers = only(Effect::Chasers);
    twoChasers.chasers = 2;
    check(litCount(runEffects(twoChasers, 25, width, height, 30, 1, sound)) > litCount(chasers), "two chasers draw more");

    // Chaser steps sit on a fixed grid of time, so the frame rate does not change the path.
    check(runEffects(only(Effect::Chasers), 25, width, height, 30, 4, sound) == chasers,
          "chasers draw the same path at 120 fps as at 30 fps");

    const Pixels bar = runEffects(only(Effect::Bar), 25, width, height, 1, 1, sound);
    bool sixteens = litCount(bar) > 0;
    for (const std::uint8_t p : bar) {
        sixteens = sixteens && p % 16 == 0;
    }
    check(sixteens, "one frame of the bar adds 16 per point");
    check(litReach(bar, width, cx, cy) <= 31.f * scale * 1.5f + 2.f, "the bar stays near the center");
    const Pixels slowBar = runEffects(only(Effect::Bar), 25, width, height, 60, 1, sound);
    check(*std::max_element(slowBar.begin(), slowBar.end()) <= 238, "the bar stops adding at 223");
    const Pixels fastBar = runEffects(only(Effect::Bar), 25, width, height, 1, 4, sound);
    check(*std::max_element(fastBar.begin(), fastBar.end()) >= 12, "at 120 fps the bar adds its share each tick");

    const Pixels dots = runEffects(only(Effect::Dots), 25, width, height, 1, 1, sound);
    check(litCount(dots) == 1, "the first frame of dots draws one dot");
    check(litCount(runEffects(only(Effect::Dots), 25, width, height, 1, 4, sound)) == 1,
          "dots move once per reference frame at 120 fps");
    check(litCount(runEffects(only(Effect::Dots), 25, width, height, 5, 1, sound)) > 1, "dots leave a trail");

    const Pixels solar = runEffects(only(Effect::Solar), 1, width, height, 10, 1, sound);
    check(litCount(solar) > 100, "solar particles land");
    check(litReach(solar, width, cx, cy) < 36.f * scale + 2.f, "solar particles land within 35 pixels at 640 wide");

    geiss::EffectSet burst;
    burst.solarBurst = true;
    check(litCount(runEffects(burst, 1, width, height, 1, 1, sound)) > 100, "map 1 can open with a solar burst");

    const Pixels grid = runEffects(only(Effect::Grid), 25, width, height, 1, 1, sound);
    const int spacing = width / 30;
    bool onRows = litCount(grid) > 0;
    for (std::size_t i = 0; i < grid.size(); ++i) {
        onRows = onRows && (grid[i] == 0 || (static_cast<int>(i) / width) % spacing == 0);
    }
    check(onRows, "the grid draws a dot every 1/30 of the width");
    check(litCount(grid) <= (width / spacing + 1) * (height / spacing + 1), "the grid is sparse");
    // After one frame scrolling left, the column at x = -1 has left and the next one has
    // come in at the right edge.
    check(only(Effect::Grid).gridDirection == 1 && grid[width - 1] > 0, "the grid fills the edge it scrolls in from");

    // The dot trail is in pixels, so clearing it leaves only the new dot.
    geiss::EffectLayer layer(3);
    const geiss::EffectSet dotsOnly = only(Effect::Dots);
    layer.startLook(dotsOnly);
    Pixels large(static_cast<std::size_t>(width) * height, 0);
    for (int i = 0; i < 10; ++i) {
        layer.draw(large.data(), width, height, dotsOnly, 25, cx, cy, 1.0, sound);
    }
    layer.clearTrail();
    Pixels small(static_cast<std::size_t>(width / 2) * (height / 2), 0);
    layer.draw(small.data(), width / 2, height / 2, dotsOnly, 25, cx / 2.f, cy / 2.f, 1.0, sound);
    check(litCount(small) == 1, "a cleared trail draws no dots from the old size");
}

void testNuclide() {
    using geiss::Effect;
    constexpr int width = 320;
    constexpr int height = 240;
    geiss::AudioFrame steady = beatFrame(false, 1.f, 0.f);
    steady.volume = 50.f;
    Pixels pixels(static_cast<std::size_t>(width) * height, 0);
    geiss::EffectLayer layer(5);
    const geiss::EffectSet effects = only(Effect::Nuclide);
    // The narrow average starts low, so the first frames of sound count as a jump.
    for (int i = 0; i < 30; ++i) {
        layer.draw(pixels.data(), width, height, effects, 25, width / 2.f, height / 2.f, 1.0, steady);
    }
    std::fill(pixels.begin(), pixels.end(), 0);
    for (int i = 0; i < 30; ++i) {
        layer.draw(pixels.data(), width, height, effects, 25, width / 2.f, height / 2.f, 1.0, steady);
    }
    check(litCount(pixels) == 0, "a steady volume drops no nuclide");
    geiss::AudioFrame jump = steady;
    jump.volume = 150.f;
    layer.draw(pixels.data(), width, height, effects, 25, width / 2.f, height / 2.f, 1.0, jump);
    check(litCount(pixels) > 20, "a jump in volume drops a ring of discs");
    const float reach = litReach(pixels, width, width / 2.f, height / 2.f);
    check(reach > 30.f * 0.5f && reach < (42.f + 11.f) * 0.5f + 2.f, "the discs sit on a ring around the center");

    // Silence keeps the picture calm: no effect draws, and a burst due in silence is dropped.
    geiss::EffectSet every;
    every.on.fill(true);
    every.chasers = 2;
    every.solarBurst = true;
    check(litCount(runEffects(every, 1, width, height, 120, 1, quietFrame())) == 0, "no effect draws in silence");

    geiss::EffectLayer late(5);
    geiss::EffectSet burstOnly;
    burstOnly.solarBurst = true;
    late.startLook(burstOnly);
    std::fill(pixels.begin(), pixels.end(), 0);
    late.draw(pixels.data(), width, height, burstOnly, 1, width / 2.f, height / 2.f, 1.0, quietFrame());
    late.draw(pixels.data(), width, height, burstOnly, 1, width / 2.f, height / 2.f, 1.0, steady);
    check(litCount(pixels) == 0, "a look that opens in silence loses its solar burst");
}

void testCenterDwindle() {
    constexpr int width = 64;
    constexpr int height = 48;
    const geiss::EffectSet none;
    auto filled = [&](int mode) {
        Pixels pixels(static_cast<std::size_t>(width) * height, 200);
        geiss::EffectLayer layer(1);
        layer.draw(pixels.data(), width, height, none, mode, 32.f, 24.f, 1.0, quietFrame());
        return pixels;
    };
    const Pixels eight = filled(8);
    check(eight[24 * width + 32] == 192 && eight[24 * width + 33] == 192 && eight[23 * width + 32] == 192,
          "mode 8 darkens the center and its neighbors by 0.96");
    check(eight[24 * width + 34] == 200 && eight[0] == 200, "mode 8 leaves the rest alone");
    const Pixels twelve = filled(12);
    check(twelve[0 * width + 31] == 183 && twelve[47 * width + 33] == 183 && twelve[0 * width + 34] == 200,
          "mode 12 darkens a three-pixel column");
    const Pixels zoom = filled(25);
    check(std::all_of(zoom.begin(), zoom.end(), [](std::uint8_t p) { return p == 200; }), "mode 25 leaves the center alone");
}

void testMode5FollowsNuclide() {
    geiss::MapParameters map = centeredMap(5);
    map.nuclide = true;
    const float with = scaleAt(60, 32, map);
    map.nuclide = false;
    const float without = scaleAt(60, 32, map);
    checkNear(with, map.f2 - map.f1 * (28.f * 10.f / 200.f * 1.7f), 0.01f, "mode 5 with the nuclide falls off with r * 1.7");
    checkNear(without, map.f2 - map.f1 * std::sqrt(28.f * 10.f / 200.f), 0.01f, "mode 5 without it falls off with sqrt(r)");
}

void testNames() {
    std::set<std::string> maps;
    for (int mode = geiss::kFirstMapMode; mode <= geiss::kLastMapMode; ++mode) {
        const std::string name = geiss::mapName(mode);
        check(!name.empty() && name != "?", "every map mode has a name");
        maps.insert(name);
    }
    check(maps.size() == static_cast<std::size_t>(geiss::kLastMapMode), "map names are unique");
    check(std::string(geiss::mapName(25)) == "Smooth zoom", "mode 25 is the smooth zoom");
    check(std::string(geiss::mapName(1)) == "Constant zoom", "mode 1 is the constant zoom");
    check(std::string(geiss::mapName(0)) == "?", "mode 0 has no name");
    check(std::string(geiss::mapName(26)) == "?", "mode 26 has no name");

    std::set<std::string> effects;
    for (int index = 0; index < geiss::kEffectCount; ++index) {
        const std::string name = geiss::effectName(static_cast<geiss::Effect>(index));
        check(!name.empty(), "every effect has a name");
        effects.insert(name);
    }
    check(effects.size() == static_cast<std::size_t>(geiss::kEffectCount), "effect names are unique");
    check(std::string(geiss::effectName(geiss::Effect::Shade)) == "Shade bobs", "Shade is shade bobs");
    check(std::string(geiss::effectName(static_cast<geiss::Effect>(geiss::kEffectCount))) == "?",
          "an unknown effect has no name");

    std::set<std::string> waves;
    for (int waveform = geiss::kFirstWaveform; waveform <= geiss::kLastWaveform; ++waveform) {
        const std::string name = geiss::waveformName(waveform);
        check(!name.empty() && name != "?", "every waveform has a name");
        waves.insert(name);
    }
    check(waves.size() == static_cast<std::size_t>(geiss::kLastWaveform), "waveform names are unique");
    check(std::string(geiss::waveformName(6)) == "Oscilloscope", "waveform 6 is the oscilloscope");
    check(std::string(geiss::waveformName(7)) == "?", "waveform 7 has no name");
}

void testSoundActive() {
    geiss::Engine engine(3);
    engine.resize(32, 24);
    check(!engine.soundActive(), "a new engine has heard nothing");
    engine.tick(1.0 / 30.0, beatFrame(false, 1.f, 0.f));
    check(engine.soundActive(), "samples make the engine active");
    geiss::AudioFrame quiet = beatFrame(false, 1.f, 0.f);
    quiet.silent = true;
    engine.tick(1.0 / 30.0, quiet);
    check(!engine.soundActive(), "silence rests the effects");
    engine.tick(1.0 / 30.0, beatFrame(false, 1.f, 0.f));
    engine.setPaused(true);
    engine.tick(1.0 / 30.0, quiet);
    check(engine.soundActive(), "pause holds the last sound state");
    engine.setPaused(false);
    engine.tick(1.0 / 30.0, geiss::AudioFrame{});
    check(!engine.soundActive(), "no samples rests the effects");
}

void testEngineEffects() {
    geiss::Engine engine(9);
    engine.resize(64, 48);
    geiss::EffectSet hide;
    hide.hideWaveform = true;
    engine.setEffects(hide);
    check(colorSum(engine.tick(1.0 / 30.0, beatFrame(false, 1.f, 0.f))) == 0, "a hidden waveform draws nothing");

    geiss::Engine locked(4);
    locked.resize(64, 48);
    locked.setLocked(true);
    const geiss::EffectSet held = locked.effects();
    for (int i = 0; i < 300; ++i) {
        locked.tick(0.1, beatFrame(false, 1.f, 0.f));
    }
    check(locked.effects().on == held.on, "lock holds the effects");

    geiss::Engine next(4);
    next.resize(64, 48);
    bool changed = false;
    const geiss::EffectSet first = next.effects();
    for (int i = 0; i < 20 && !changed; ++i) {
        next.nextVisual();
        changed = next.effects().on != first.on;
    }
    check(changed, "Next picks new effects");

    bool nuclideMatches = true;
    for (int i = 0; i < 300; ++i) {
        next.nextVisual();
        nuclideMatches = nuclideMatches && next.map().nuclide == next.effects().has(geiss::Effect::Nuclide);
    }
    check(nuclideMatches, "the map knows whether its look has the nuclide");
}

// Average red after six seconds of silence that follow two seconds of sound, at `fps`.
double silentFade(double fps) {
    geiss::Engine engine = engineShowing(25, 160, 90);
    engine.setLocked(true);
    const geiss::AudioFrame loud = beatFrame(false, 1.f, 0.f);
    for (int i = 0; i < static_cast<int>(2 * fps); ++i) {
        engine.tick(1.0 / fps, loud);
    }
    geiss::AudioFrame quiet;
    quiet.silent = true;
    geiss::FrameView frame{};
    for (int i = 0; i < static_cast<int>(6 * fps); ++i) {
        frame = engine.tick(1.0 / fps, quiet);
    }
    double red = 0;
    const std::size_t pixels = static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    for (std::size_t i = 0; i < pixels; ++i) {
        red += frame.rgba[i * 4];
    }
    return red / static_cast<double>(pixels);
}

void testSilenceFadesAtAnyFrameRate() {
    check(silentFade(30) < 1.0, "the picture fades out in silence at 30 fps");
    check(silentFade(60) < 1.0, "the picture fades out in silence at 60 fps");
    check(silentFade(120) < 1.0, "the picture fades out in silence at 120 fps");
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape): a test may abort on a failed allocation
    testMode25PullsTowardCenter();
    testWarpDecayAndSample();
    testAnalysis();
    testWaveWindow();
    testEnginePauseDrawsALine();
    testBassLevel();
    testBeatDetector();
    testSilenceHoldUsesTime();
    testBeatBrightness();
    testLookWaitsForBigBeat();
    testVolumeScale();
    testWaveBrightnessFollowsVolume();
    testSlideShiftPicks();
    testSlideShiftOnPeaks();
    testPeakRelaxes();
    testWarpShift();
    testBassAverage();
    testFadeMatchesAtAnyFrameRate();
    testSilenceFadesAtAnyFrameRate();
    testEngineSlideShift();
    testPaletteCurves();
    testBuildPalette();
    testBlendPalettes();
    testRandomPaletteRecipes();
    testEnginePalettes();
    testEnginePaletteBlend();
    testPaletteSilentGamma();
    testEveryModeBuildsAFiniteMap();
    testModeDampingAndDecay();
    testNewModeShapes();
    testLookPicking();
    testNewWaveforms();
    testLineSitsLowerOverScroll();
    testLowModeShapes();
    testDitherParity();
    testTallFrameKeepsScalesPositive();
    testRandomLeavesZero();
    testModeEffectsTable();
    testRandomEffectsRespectLimits();
    testEachEffectDraws();
    testNuclide();
    testCenterDwindle();
    testMode5FollowsNuclide();
    testEngineEffects();
    testNames();
    testSoundActive();
    if (failures != 0) {
        std::cerr << failures << " failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "ok\n";
    return EXIT_SUCCESS;
}
