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
#include "geiss/color.hpp"
#include "geiss/engine.hpp"
#include "geiss/map.hpp"
#include "geiss/palette.hpp"
#include "geiss/shift.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <cstdint>
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

// A simulation pixel buffer for the tests: geiss::kPixelBytes per pixel. Indexing gives a
// pixel's brightest channel, which is its level for the effects that draw grey.
class Pixels {
public:
    Pixels() = default;
    Pixels(std::size_t count, std::uint8_t fill) : bytes_(count * geiss::kPixelBytes, 0) {
        for (std::size_t i = 0; i < count; ++i) {
            set(i, {fill, fill, fill});
        }
    }

    std::uint8_t* data() { return bytes_.data(); }
    const std::uint8_t* data() const { return bytes_.data(); }
    std::size_t size() const { return bytes_.size() / geiss::kPixelBytes; }
    std::uint8_t operator[](std::size_t i) const {
        const std::uint8_t* p = &bytes_[i * geiss::kPixelBytes];
        return std::max({p[0], p[1], p[2]});
    }
    geiss::Rgb rgb(std::size_t i) const {
        const std::uint8_t* p = &bytes_[i * geiss::kPixelBytes];
        return {p[0], p[1], p[2]};
    }
    // The held level: how much of the brightness is held to SDR.
    std::uint8_t held(std::size_t i) const { return bytes_[i * geiss::kPixelBytes + geiss::kHeldByte]; }
    void setHeld(std::size_t i, std::uint8_t level) { bytes_[i * geiss::kPixelBytes + geiss::kHeldByte] = level; }
    // The held share the presenter gets for pixel `i`: 255 when all of its brightness is held.
    std::uint8_t shownHeld(std::size_t i) const { return geiss::displayPixel(&bytes_[i * geiss::kPixelBytes])[3]; }
    void set(std::size_t i, geiss::Rgb color) {
        std::uint8_t* p = &bytes_[i * geiss::kPixelBytes];
        p[0] = static_cast<std::uint8_t>(color.r);
        p[1] = static_cast<std::uint8_t>(color.g);
        p[2] = static_cast<std::uint8_t>(color.b);
    }
    std::vector<std::uint8_t> levels() const {
        std::vector<std::uint8_t> out(size());
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = (*this)[i];
        }
        return out;
    }
    void clear() { std::fill(bytes_.begin(), bytes_.end(), 0); }
    friend bool operator==(const Pixels&, const Pixels&) = default;

private:
    std::vector<std::uint8_t> bytes_;
};

std::uint8_t maxLevel(const Pixels& pixels) {
    const std::vector<std::uint8_t> levels = pixels.levels();
    return *std::max_element(levels.begin(), levels.end());
}

template <typename Predicate>
bool allLevels(const Pixels& pixels, Predicate predicate) {
    const std::vector<std::uint8_t> levels = pixels.levels();
    return std::all_of(levels.begin(), levels.end(), predicate);
}

std::vector<float> toneSamples(double hz, float amplitude, double rate, int frames) {
    std::vector<float> tone(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        tone[static_cast<std::size_t>(i)] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * i / rate));
    }
    return tone;
}

std::size_t strongestBand(const std::array<float, geiss::kFourierDetail>& power) {
    return static_cast<std::size_t>(std::max_element(power.begin(), power.end()) - power.begin());
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
    Pixels src(8, 0);
    Pixels dst(8, 0);
    const std::array<int, 8> levels = {10, 20, 0, 0, 30, 40, 0, 0};
    for (std::size_t i = 0; i < levels.size(); ++i) {
        src.set(i, {levels.at(i), levels.at(i), levels.at(i)});
    }
    std::vector<geiss::SourcePoint> map(8);
    for (int i = 0; i < 8; ++i) {
        map[static_cast<std::size_t>(i)] = {0.f, 0.f};
    }
    geiss::warpPixels(src.data(), dst.data(), width, height, map.data(), 1.f, 0.5f);
    check(dst[0] == 5, "identity sample at 0,0 decays by half");
    check(dst[1] == 5, "a target of 0,0 copies the decayed corner");

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            map[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = {static_cast<float>(x), static_cast<float>(y)};
        }
    }
    src.set(0, {100, 60, 20});
    src.data()[geiss::kHeldByte] = 100;
    geiss::warpPixels(src.data(), dst.data(), width, height, map.data(), 0.f, 0.5f);
    check(dst.rgb(0) == geiss::Rgb{50, 30, 10}, "step 0 samples the pixel itself, each channel alike");
    check(dst.data()[geiss::kHeldByte] == 50, "the held level warps and darkens with the colors");

    // Half a pixel to the right averages two pixels channel by channel.
    src.set(0, {100, 0, 200});
    src.set(1, {0, 100, 0});
    for (auto& point : map) {
        point.x += 0.5f;
    }
    geiss::warpPixels(src.data(), dst.data(), width, height, map.data(), 1.f, 1.f);
    check(dst.rgb(0) == geiss::Rgb{50, 50, 100}, "the bilinear weights apply to each channel");
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

void testWrapColumn() {
    // In range, the column is returned as is. Outside, it wraps by width - 1 like fmod did.
    check(geiss::wrapColumn(0.f, 10) == 0.f, "column 0 stays");
    check(geiss::wrapColumn(4.25f, 10) == 4.25f, "an in-range column stays");
    checkNear(geiss::wrapColumn(9.f, 10), 0.f, 1e-6f, "the last column wraps to the first, as the original's FXW-1 step did");
    checkNear(geiss::wrapColumn(11.5f, 10), 2.5f, 1e-5f, "a column past the right edge wraps");
    checkNear(geiss::wrapColumn(-0.5f, 10), 8.5f, 1e-5f, "a column past the left edge wraps");
    checkNear(geiss::wrapColumn(-20.f, 10), 7.f, 1e-5f, "a far-left column wraps more than once");
    check(geiss::wrapColumn(3.f, 1) == 0.f, "a one-column frame has nowhere to go");
    for (int step = -81; step < 81; ++step) {
        const float value = static_cast<float>(step) * 0.37f;
        float reference = std::fmod(value, 9.f);
        if (reference < 0.f) {
            reference += 9.f;
        }
        check(geiss::wrapColumn(value, 10) == reference, "wrapColumn matches the fmod wrap everywhere");
    }
}

// Runs the bands last to first and counts the calls, so a result that depends on the
// order, or a band that is skipped or run twice, shows up.
struct ReverseBands {
    std::vector<int> calls;

    static void run(void* host, int count, void* job, geiss::BandRunner::Band band) {
        auto& self = *static_cast<ReverseBands*>(host);
        self.calls.assign(static_cast<std::size_t>(count), 0);
        for (int index = count - 1; index >= 0; --index) {
            ++self.calls[static_cast<std::size_t>(index)];
            band(job, index);
        }
    }

    geiss::BandRunner runner() { return geiss::BandRunner{&ReverseBands::run, this}; }

    bool eachOnce() const {
        return !calls.empty() && std::all_of(calls.begin(), calls.end(), [](int c) { return c == 1; });
    }
};

void testBandsMatchSerial() {
    check(geiss::bandCount(540) == 16, "a full frame splits into 16 bands");
    check(geiss::bandCount(5) == 5, "a short frame has a band per row");
    check(geiss::bandCount(0) == 1, "an empty frame still has one band");
    check(geiss::bandFirstRow(0, 16, 540) == 0 && geiss::bandFirstRow(16, 16, 540) == 540, "the bands cover every row");

    constexpr int width = 97;
    constexpr int height = 61;
    for (const int mode : {5, 6, 7, 10, 25}) {
        geiss::Random random;
        random.state = 11u + static_cast<std::uint32_t>(mode);
        const geiss::MapParameters map = geiss::randomMapParameters(mode, width, height, random);
        std::vector<geiss::SourcePoint> serial;
        std::vector<geiss::SourcePoint> banded;
        geiss::buildMap(serial, width, height, map);
        ReverseBands bands;
        geiss::buildMap(banded, width, height, map, bands.runner());
        check(bands.eachOnce(), "buildMap runs each band once");
        bool sameMap = serial.size() == banded.size();
        for (std::size_t i = 0; sameMap && i < serial.size(); ++i) {
            sameMap = serial[i].x == banded[i].x && serial[i].y == banded[i].y;
        }
        check(sameMap, "a banded map matches the serial map");

        Pixels src(static_cast<std::size_t>(width) * height, 0);
        for (std::size_t i = 0; i < src.size(); ++i) {
            src.set(i, {static_cast<int>((i * 37u + 11u) % 256u), static_cast<int>((i * 53u) % 256u), static_cast<int>((i * 17u + 5u) % 256u)});
        }
        Pixels serialOut(src.size(), 0);
        Pixels bandedOut(src.size(), 0);
        geiss::warpPixels(src.data(), serialOut.data(), width, height, serial.data(), 0.5f, 0.97f, 1.5f, -0.5f, 9u);
        geiss::warpPixels(src.data(), bandedOut.data(), width, height, serial.data(), 0.5f, 0.97f, 1.5f, -0.5f, 9u,
                          bands.runner());
        check(bands.eachOnce(), "warpPixels runs each band once");
        check(serialOut == bandedOut, "a banded warp matches the serial warp");
    }

    // A whole engine gives the same pictures with a band runner as without one.
    geiss::Engine plain(21);
    geiss::Engine banded(21);
    ReverseBands bands;
    banded.setBandRunner(bands.runner());
    plain.resize(80, 45);
    banded.resize(80, 45);
    geiss::AudioFrame audio;
    audio.volume = 100.f;
    audio.averageVolume = 100.f;
    audio.samples.resize(80);
    for (int i = 0; i < 80; ++i) {
        audio.samples[static_cast<std::size_t>(i)] = std::sin(static_cast<float>(i) * 0.3f);
    }
    bool samePictures = true;
    for (int frame = 0; frame < 40; ++frame) {
        if (frame % 10 == 0) {
            plain.nextVisual();
            banded.nextVisual();
        }
        const geiss::FrameView a = plain.tick(1.0 / 60.0, audio);
        const geiss::FrameView b = banded.tick(1.0 / 60.0, audio);
        samePictures = samePictures && std::equal(a.rgba, a.rgba + static_cast<std::size_t>(a.width) * a.height * 4, b.rgba);
    }
    check(samePictures, "an engine with bands draws the same pictures");
}

void testFrameChanged() {
    geiss::Engine engine(5);
    engine.resize(32, 24);
    geiss::AudioFrame audio;
    check(engine.tick(1.0 / 60.0, audio).changed, "a moving tick changes the frame");
    engine.setPaused(true);
    check(!engine.tick(1.0 / 60.0, audio).changed, "a paused tick leaves the frame as it was");
    engine.nextVisual();
    check(!engine.tick(1.0 / 60.0, audio).changed, "next while paused waits for the picture to move");
    engine.resize(40, 30);
    check(engine.tick(1.0 / 60.0, audio).changed, "a resize while paused changes the frame once");
    check(!engine.tick(1.0 / 60.0, audio).changed, "and only once");
    engine.setPaused(false);
    check(engine.tick(1.0 / 60.0, audio).changed, "resuming changes the frame again");
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

struct RecordingWarp {
    std::vector<geiss::WarpJob> jobs;
    static void run(void* host, const geiss::WarpJob& job) {
        auto& self = *static_cast<RecordingWarp*>(host);
        self.jobs.push_back(job);
        geiss::warpPixels(job.src, job.dst, job.width, job.height, job.map, job.step, job.decay, job.shiftX, job.shiftY,
                          job.ditherSeed);
    }
    geiss::WarpRunner runner() { return geiss::WarpRunner{&RecordingWarp::run, this}; }
};

void testWarpRunner() {
    geiss::AudioFrame audio;
    audio.volume = 100.f;
    audio.averageVolume = 100.f;
    audio.samples.resize(80);
    for (int i = 0; i < 80; ++i) {
        audio.samples[static_cast<std::size_t>(i)] = std::sin(static_cast<float>(i) * 0.3f);
    }
    geiss::Engine plain(21);
    geiss::Engine hosted(21);
    RecordingWarp warp;
    hosted.setWarpRunner(warp.runner());
    plain.resize(80, 45);
    hosted.resize(80, 45);
    bool same = true;
    for (int frame = 0; frame < 30; ++frame) {
        if (frame % 10 == 0) {
            plain.nextVisual();
            hosted.nextVisual();
        }
        const geiss::FrameView a = plain.tick(1.0 / 60.0, audio);
        const geiss::FrameView b = hosted.tick(1.0 / 60.0, audio);
        same = same && std::equal(a.rgba, a.rgba + static_cast<std::size_t>(a.width) * a.height * 4, b.rgba);
    }
    check(same, "a host warp that runs warpPixels draws the same pictures");
    check(warp.jobs.size() == 30, "the host warp runs once per moving tick");

    const geiss::EngineBuffers buffers = hosted.buffers();
    const geiss::WarpJob& last = warp.jobs.back();
    check(last.width == 80 && last.height == 45 && last.map == buffers.map, "a warp job carries the size and the map");
    // The tick swapped the buffers after the warp, so the last destination is now the front.
    check(last.dst == buffers.pixelsA && last.src == buffers.pixelsB, "a warp reads one shared buffer and writes the other");
    check(warp.jobs[0].ditherSeed != warp.jobs[1].ditherSeed, "each warp gets a new rounding pattern");
    check(last.decay > 0.f && last.decay < 1.f && last.step > 0.f, "a warp job carries the decay and the step");
    check(std::all_of(warp.jobs.begin(), warp.jobs.end(),
                      [](const geiss::WarpJob& job) {
                          return job.step >= 0.f && job.step <= 1.f && job.decay >= 0.f && job.decay <= 1.f;
                      }),
          "a warp job's step and decay are clamped as warpPixels clamps them");

    hosted.setPaused(true);
    hosted.tick(1.0 / 60.0, audio);
    check(warp.jobs.size() == 30, "a paused tick runs no warp");
}

// A warp that finishes only when the engine waits for it. Until then the destination holds
// junk, so a picture that read it early would show.
struct DeferredWarp {
    std::vector<geiss::WarpJob> jobs;
    int waits = 0;
    bool pending = false;
    static void run(void* host, const geiss::WarpJob& job) {
        auto& self = *static_cast<DeferredWarp*>(host);
        self.jobs.push_back(job);
        self.pending = true;
        std::fill(job.dst, job.dst + static_cast<std::size_t>(job.width) * job.height * geiss::kPixelBytes,
                  std::uint8_t{0xAB});
    }
    static void wait(void* host) {
        auto& self = *static_cast<DeferredWarp*>(host);
        ++self.waits;
        if (self.pending) {
            const geiss::WarpJob& job = self.jobs.back();
            geiss::warpPixels(job.src, job.dst, job.width, job.height, job.map, job.step, job.decay, job.shiftX,
                              job.shiftY, job.ditherSeed);
            self.pending = false;
        }
    }
    geiss::WarpRunner runner() { return geiss::WarpRunner{&DeferredWarp::run, this, &DeferredWarp::wait}; }
};

void testPipelinedWarp() {
    geiss::AudioFrame audio;
    audio.volume = 100.f;
    audio.averageVolume = 100.f;
    audio.samples.resize(80);
    for (int i = 0; i < 80; ++i) {
        audio.samples[static_cast<std::size_t>(i)] = std::sin(static_cast<float>(i) * 0.3f);
    }
    // With a steady time step and steady sound, starting each warp a frame early changes nothing.
    geiss::Engine plain(23);
    geiss::Engine piped(23);
    DeferredWarp warp;
    piped.setWarpRunner(warp.runner());
    plain.resize(80, 45);
    piped.resize(80, 45);
    plain.setSlideShiftAllowed(false);
    piped.setSlideShiftAllowed(false);
    bool same = true;
    for (int frame = 0; frame < 30; ++frame) {
        const geiss::FrameView a = plain.tick(1.0 / 60.0, audio);
        const geiss::FrameView b = piped.tick(1.0 / 60.0, audio);
        same = same && std::equal(a.rgba, a.rgba + static_cast<std::size_t>(a.width) * a.height * 4, b.rgba);
        piped.startNextWarp(audio);
    }
    check(same, "a warp started at the end of the frame before draws the same pictures");
    check(warp.jobs.size() == 31, "each frame's warp starts once, the first in its own tick");
    check(warp.pending && warp.waits == 30, "the warp started after a tick runs until the next tick waits for it");

    // Without wait, the next tick warps itself.
    geiss::Engine synchronous(23);
    RecordingWarp recording;
    synchronous.setWarpRunner(recording.runner());
    synchronous.resize(80, 45);
    synchronous.tick(1.0 / 60.0, audio);
    synchronous.startNextWarp(audio);
    check(recording.jobs.size() == 1, "a warp runner without wait starts no warp early");

    // Before the first moving tick there is no time step to warp with.
    geiss::Engine fresh(23);
    DeferredWarp unused;
    fresh.setWarpRunner(unused.runner());
    fresh.resize(80, 45);
    fresh.startNextWarp(audio);
    check(unused.jobs.empty(), "no warp starts before the first moving tick");

    // Pause holds the started warp, and starts none.
    const std::size_t started = warp.jobs.size();
    piped.setPaused(true);
    piped.tick(1.0 / 60.0, audio);
    piped.startNextWarp(audio);
    check(warp.jobs.size() == started && !warp.pending, "a paused engine finishes the started warp and starts none");
    piped.setPaused(false);
    piped.tick(1.0 / 60.0, audio);
    check(warp.jobs.size() == started, "resuming shows the warp started before the pause");

    // A new look and a resize wait for the running warp before they rewrite its map and frames.
    piped.startNextWarp(audio);
    check(warp.pending, "a warp is running");
    piped.nextVisual();
    check(!warp.pending, "a new look waits for the running warp before building its map");
    piped.tick(1.0 / 60.0, audio);
    piped.startNextWarp(audio);
    piped.resize(64, 36);
    check(!warp.pending, "a resize waits for the running warp before replacing the frames");
    const std::size_t beforeResizedTick = warp.jobs.size();
    const geiss::FrameView resized = piped.tick(1.0 / 60.0, audio);
    check(warp.jobs.size() == beforeResizedTick + 1 && warp.jobs.back().width == 64 && resized.width == 64,
          "the first tick after a resize warps the new size itself");
    piped.startNextWarp(audio);
    piped.finishWarp();
    check(!warp.pending, "finishWarp waits for the running warp");
}

void testSharedBuffers() {
    geiss::Engine engine(3);
    engine.resize(100, 60);
    const geiss::EngineBuffers first = engine.buffers();
    const auto aligned = [](const void* pointer) {
        return reinterpret_cast<std::uintptr_t>(pointer) % geiss::kBufferPage == 0;
    };
    check(aligned(first.pixelsA) && aligned(first.pixelsB) && aligned(first.map), "the frame buffers are page-aligned");
    check(first.pixelBytes % geiss::kBufferPage == 0 && first.pixelBytes >= std::size_t{100} * 60 * geiss::kPixelBytes,
          "the pixel buffers span whole pages");
    check(first.mapBytes % geiss::kBufferPage == 0 && first.mapBytes >= std::size_t{100} * 60 * sizeof(geiss::SourcePoint),
          "the map spans whole pages");
    engine.nextVisual();
    engine.tick(1.0 / 60.0, geiss::AudioFrame{});
    const geiss::EngineBuffers ticked = engine.buffers();
    check(ticked.generation == first.generation && ticked.map == first.map, "Next and ticks keep the buffers");
    check((ticked.pixelsA == first.pixelsB && ticked.pixelsB == first.pixelsA), "a tick swaps the two pixel buffers");
    engine.resize(120, 70);
    check(engine.buffers().generation != first.generation, "a resize makes new buffers");
    engine.resize(120, 70);
    const auto again = engine.buffers().generation;
    engine.resize(120, 70);
    check(engine.buffers().generation == again, "the same size keeps them");
}

void testHostExpands() {
    geiss::Engine cpu(9);
    geiss::Engine host(9);
    host.setHostExpands(true);
    cpu.resize(64, 36);
    host.resize(64, 36);
    const geiss::AudioFrame audio = beatFrame(false, 1.f, 0.f);
    geiss::FrameView a;
    geiss::FrameView b;
    for (int i = 0; i < 5; ++i) {
        a = cpu.tick(1.0 / 30.0, audio);
        b = host.tick(1.0 / 30.0, audio);
    }
    check(b.rgba == nullptr && b.pixels != nullptr && b.changed, "a host-expanded view carries the pixels instead of RGBA");
    check(a.pixels != nullptr, "every view carries the pixels");
    bool same = true;
    for (std::size_t i = 0; i < static_cast<std::size_t>(b.width) * b.height; ++i) {
        const auto shown = geiss::displayPixel(b.pixels + i * geiss::kPixelBytes);
        same = same && std::equal(shown.begin(), shown.end(), a.rgba + i * 4);
    }
    check(same, "host-expanded pixels show what the engine's own expansion does");

    // Handing the expansion back after a resize gives a complete RGBA view again.
    host.resize(72, 40);
    host.setHostExpands(false);
    host.setPaused(true);
    const geiss::FrameView back = host.tick(1.0 / 30.0, audio);
    bool expanded = back.rgba != nullptr && back.changed;
    for (std::size_t i = 0; expanded && i < static_cast<std::size_t>(back.width) * back.height; ++i) {
        const auto shown = geiss::displayPixel(back.pixels + i * geiss::kPixelBytes);
        expanded = std::equal(shown.begin(), shown.end(), back.rgba + i * 4);
    }
    check(expanded, "turning host expansion off expands the current frame again");
    host.setPaused(false);
    const geiss::FrameView moving = host.tick(1.0 / 30.0, audio);
    check(moving.rgba != nullptr && moving.width == 72, "a moving tick after it fills the RGBA view");
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
    Pixels src(std::size_t{width} * height, 0);
    Pixels dst(src.size(), 0);
    src.set(std::size_t{5} * width + 5, {200, 200, 200});
    geiss::warpPixels(src.data(), dst.data(), width, height, identity.data(), 1.f, 1.f, 1.f, 0.f);
    check(dst[std::size_t{5} * width + 4] == 200, "a positive sideways shift slides the picture left");
    geiss::warpPixels(src.data(), dst.data(), width, height, identity.data(), 1.f, 1.f, 0.f, -1.f);
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
    Pixels a(std::size_t{width} * height, 100);
    Pixels b(a.size(), 0);
    const auto decay = static_cast<float>(std::pow(255.0 / 256.0, 30.0 / fps));
    for (int i = 0; i < fps; ++i) {
        geiss::warpPixels(a.data(), b.data(), width, height, identity.data(), 1.f, decay, 0.f, 0.f,
                          static_cast<std::uint32_t>(i + 1));
        std::swap(a, b);
    }
    double sum = 0;
    for (const std::uint8_t value : a.levels()) {
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

void testDisplayPixel() {
    const std::array<std::uint8_t, 4> dark{0, 0, 0, 0};
    check(geiss::displayPixel(dark.data()) == dark, "black shows black");
    const std::array<std::uint8_t, 4> dim{10, 64, 250, 0};
    const auto shown = geiss::displayPixel(dim.data());
    check(shown[0] == 10 && shown[1] == 64 && shown[2] == 250, "the display passes the stored channels on for the palette");
    check(shown[3] == 0, "a pixel with nothing held shows no held share");

    // The held share is the held level over the brightest channel.
    const std::array<std::uint8_t, 4> held{30, 168, 250, 250};
    const auto texture = geiss::displayPixel(held.data());
    check(texture[0] == 30 && texture[1] == 168 && texture[2] == 250, "a held pixel shows the same channels");
    check(texture[3] == 255, "a pixel held to SDR is fully held");
    const std::array<std::uint8_t, 4> half{0, 0, 250, 125};
    check(geiss::displayPixel(half.data())[3] == 127, "a half-held pixel shows half");
    const std::array<std::uint8_t, 4> over{0, 0, 100, 255};
    check(geiss::displayPixel(over.data())[3] == 255, "a held level above the brightness is fully held");

    // A line or soft spot that brightens a held pixel takes it over, so it glows again.
    Pixels spot(25, 150);
    for (std::size_t i = 0; i < spot.size(); ++i) {
        spot.setHeld(i, 150);
    }
    geiss::plotSoftSpot(spot.data(), 5, 5, 2, 2, {250, 250, 250});
    check(spot.held(12) == 0 && spot.shownHeld(12) == 0, "a line over a held texture takes it over");
    check(spot.held(0) == 150, "a spot too faint to brighten a pixel leaves it held");

    // A line that only lifts a dimmer channel of a texture leaves it held.
    Pixels edge(1, 0);
    edge.set(0, {200, 20, 20});
    edge.setHeld(0, 200);
    geiss::plotSoftSpot(edge.data(), 1, 1, 0, 0, {0, 0, 150});
    check(edge.rgb(0) == geiss::Rgb{200, 20, 150} && edge.held(0) == 200 && edge.shownHeld(0) == 255,
          "a blue line over a red texture leaves the texture held");
}

void testPalettes() {
    check(geiss::paletteCount() == 355, "all of Geiss HDR's saved palettes are ported");
    bool shaped = true;
    for (int i = 0; i < geiss::paletteCount(); ++i) {
        const geiss::Palette& p = geiss::savedPalette(i);
        shaped = shaped && p.count >= 2 && p.count <= geiss::kMaxPaletteStops;
        const geiss::LinearRgb black = geiss::samplePalette(p, 0.f);
        shaped = shaped && black.r == 0.f && black.g == 0.f && black.b == 0.f;
    }
    check(shaped, "every palette has 2 to 8 stops and starts at black");
    const geiss::Palette& first = geiss::savedPalette(0);
    const geiss::LinearRgb top = geiss::samplePalette(first, 1.f);
    checkNear(top.r, 2.7884f, 1e-4f, "the ramp ends on its last stop");
    const geiss::LinearRgb between = geiss::samplePalette(first, 1.5f / 7.f);
    checkNear(between.g, (0.1137f + 0.3644f) / 2.f, 1e-4f, "the ramp interpolates between stops");

    // Reference values from Geiss HDR's own HdrToSdr and ReduceHdrHeadroom.
    const geiss::LinearRgb mid = geiss::hdrToSdr(0.5f, {1.4f, 1.2f, 0.2f}, 2.9f);
    checkNear(mid.r, 0.808190f, 1e-4f, "hdrToSdr matches Geiss HDR in the midtones (red)");
    checkNear(mid.b, 0.133642f, 1e-4f, "hdrToSdr matches Geiss HDR in the midtones (blue)");
    const geiss::LinearRgb hot = geiss::hdrToSdr(1.f, {2.9f, 2.5f, 0.4f}, 2.9f);
    check(hot.r == 1.f && hot.g == 1.f, "hdrToSdr clamps to SDR white");
    checkNear(hot.b, 0.319543f, 1e-4f, "hdrToSdr fades the top toward white");
    const geiss::LinearRgb shadow = geiss::hdrToSdr(0.1f, {0.1f, 0.05f, 0.02f}, 2.9f);
    checkNear(shadow.r, 0.054373f, 1e-4f, "hdrToSdr darkens deep shadows");
    const geiss::LinearRgb roomy = geiss::reduceHdrHeadroom(2.5f, 1.f, {2.9f, 2.5f, 0.4f}, 2.9f);
    checkNear(roomy.r, 2.5f, 1e-4f, "reduceHdrHeadroom fills the headroom");
    checkNear(roomy.g, 2.321126f, 1e-4f, "reduceHdrHeadroom matches Geiss HDR");

    geiss::PaletteLut sdr{};
    geiss::buildPaletteLut(first, first, 0.f, 1.f, sdr);
    check(sdr[0][0] == 0.f && sdr[0][1] == 0.f && sdr[0][2] == 0.f && sdr[0][3] == 1.f, "the table starts at black");
    bool withinSdr = true;
    for (const auto& entry : sdr) {
        withinSdr = withinSdr && entry[0] <= 1.f && entry[1] <= 1.f && entry[2] <= 1.f;
    }
    check(withinSdr, "an SDR screen's table stays within SDR white");
    geiss::PaletteLut hdr{};
    geiss::buildPaletteLut(first, first, 0.f, 2.5f, hdr);
    check(hdr[255][0] > 1.f && hdr[255][0] <= 2.5f, "an HDR screen's table glows at the top, within the headroom");

    // A blend mixes the two ramps before the remap.
    const geiss::Palette& second = geiss::savedPalette(1);
    geiss::PaletteLut a{};
    geiss::PaletteLut b{};
    geiss::PaletteLut half{};
    geiss::buildPaletteLut(first, first, 0.f, 4.f, a);
    geiss::buildPaletteLut(second, second, 0.f, 4.f, b);
    geiss::buildPaletteLut(first, second, 0.5f, 4.f, half);
    check(half[128][1] > std::min(a[128][1], b[128][1]) && half[128][1] < std::max(a[128][1], b[128][1]),
          "a half blend lies between the two palettes");
}

void testPaletteChanges() {
    geiss::Engine engine(3);
    const geiss::AudioFrame audio = beatFrame(false, 1.f, 0.f);
    check(engine.palette() == 0 && engine.paletteBlend() == 1.f, "the window opens on the first palette");
    engine.nextVisual();
    const int picked = engine.palette();
    check(picked != 0 && engine.paletteBlend() == 0.f, "Next starts a fade to another palette");
    engine.tick(1.0 / 30.0, audio);
    const float early = engine.paletteBlend();
    check(early > 0.f && early < 0.05f, "the fade moves with time");
    engine.setPaused(true);
    engine.tick(1.0, audio);
    check(engine.paletteBlend() == early, "pause freezes the fade");
    engine.setPaused(false);
    engine.setLocked(true);
    for (int i = 0; i < 30 * 30; ++i) {
        engine.tick(1.0 / 30.0, audio);
    }
    check(engine.palette() == picked && engine.paletteBlend() == 1.f, "lock holds the palette after its fade");

    // The table follows the fade and the headroom, and is reused while neither moves.
    const geiss::PaletteLut& lut = engine.paletteLut(1.f);
    geiss::PaletteLut expected{};
    geiss::buildPaletteLut(geiss::savedPalette(picked), geiss::savedPalette(picked), 0.f, 1.f, expected);
    check(lut == expected, "a finished fade shows the new palette");
    const float before = lut[255][0];
    const float glowing = engine.paletteLut(2.5f)[255][0];
    check(glowing > before || before == 0.f, "more headroom brightens the top of the table");
}

// Ticks one frame of a waveform-1 line over a black, effect-free picture.
geiss::FrameView lineFrame(geiss::Engine& engine, const geiss::AudioFrame& audio) {
    engine.resize(64, 48);
    engine.setEffects({});
    engine.setSlideShiftAllowed(false);
    engine.setIdleWave(false);
    return engine.tick(1.0 / 30.0, audio);
}

std::array<long, 3> channelSums(const geiss::FrameView& frame) {
    std::array<long, 3> sums{};
    const std::size_t pixels = static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    for (std::size_t i = 0; i < pixels; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            sums.at(c) += frame.rgba[i * 4 + c];
        }
    }
    return sums;
}

void testWaveFollowsTheSpectrum() {
    constexpr double rate = 48000.0;
    auto colored = [&](double hz) {
        geiss::AudioFrame audio = beatFrame(false, 1.f, 0.f);
        const auto tone = toneSamples(hz, 0.5f, rate, 256);
        audio.power = geiss::fourierPower(tone.data(), 256, rate);
        geiss::Engine engine(1);
        return channelSums(lineFrame(engine, audio));
    };
    const auto bass = colored(40.0);
    check(bass[0] > bass[1] && bass[0] > bass[2], "a bass-heavy wave draws red");
    const auto treble = colored(8000.0);
    check(treble[2] > treble[0] && treble[2] > treble[1], "a treble-heavy wave draws blue");

    // Without spectrum power the line takes the drift color, which every channel shares in.
    geiss::Engine plain(1);
    const geiss::FrameView frame = lineFrame(plain, beatFrame(false, 1.f, 0.f));
    const auto drift = channelSums(frame);
    check(drift[0] > 0 && drift[1] > 0 && drift[2] > 0, "a wave without spectrum power still draws in color");
    bool unheld = true;
    const std::size_t pixels = static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    for (std::size_t i = 0; i < pixels; ++i) {
        unheld = unheld && frame.rgba[i * 4 + 3] == 0;
    }
    check(unheld, "a line over black is not held to SDR");
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
        check(map.mode == (geiss::isMapMode(mode) ? mode : 25), "random parameters keep the mode");
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

void testModeDamping() {
    for (int mode = geiss::kFirstMapMode; mode <= geiss::kLastMapMode; ++mode) {
        if (!geiss::isMapMode(mode)) {
            continue;
        }
        geiss::Random random{3};
        const geiss::MapParameters map = geiss::randomMapParameters(mode, 64, 48, random);
        const bool dampened = mode <= 16 && mode != 3 && mode != 6;
        checkNear(map.damping, dampened ? 0.5f : 1.f, 0.f, "mode_motion_dampened halves modes 1-16 but 3 and 6");
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
    bool noSplitter = true;
    for (int waveform = geiss::kFirstWaveform; waveform <= geiss::kLastWaveform; ++waveform) {
        noSplitter = noSplitter && !geiss::lookAllowed(12, waveform);
    }
    check(noSplitter, "mode 12, the sideways splitter, is never a look");
    check(!geiss::isMapMode(12) && geiss::isMapMode(11) && geiss::isMapMode(13), "only mode 12 is left out");
    geiss::Random retired{5};
    check(geiss::randomMapParameters(12, 64, 48, retired).mode == 25, "asking for mode 12 gives the smooth zoom");
    check(geiss::randomMapParameters(0, 64, 48, retired).mode == 1, "a mode below 1 clamps to mode 1");
    check(geiss::randomMapParameters(26, 64, 48, retired).mode == 25, "a mode above 25 clamps to mode 25");
    const geiss::ModeEffects splitterInfo = geiss::modeEffects(12);
    const geiss::ModeEffects zoomInfo = geiss::modeEffects(25);
    check(splitterInfo.weights == zoomInfo.weights && splitterInfo.centerDwindle == zoomInfo.centerDwindle &&
              splitterInfo.minEffects == zoomInfo.minEffects && splitterInfo.maxEffects == zoomInfo.maxEffects &&
              splitterInfo.solarMax == zoomInfo.solarMax,
          "mode 12 gets the smooth zoom's effect info");
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
        everyMode = everyMode && (modes[static_cast<std::size_t>(mode)] > 0) == geiss::isMapMode(mode);
    }
    check(everyMode, "every map mode comes up, and mode 12 never does");
    check(modes[5] > modes[1] * 3 / 2 && modes[7] > modes[1] * 3 / 2, "modes 5 and 7 come up more often");
    check(waves[6] < waves[1] && waves[5] > 0 && waves[3] > 0, "waveform 6 comes up less often");
}

// A fresh engine moved to `waveform` by Next, ticked once with a silent-looking flat wave.
// No slide shift, so a test sees only what it draws.
void pinLook(geiss::Engine& engine) { engine.setSlideShiftAllowed(false); }

geiss::Engine engineDrawing(int waveform, int width, int height) {
    geiss::Engine engine(17);
    engine.resize(width, height);
    // Mode 6 draws its edges with sound, so it is skipped to leave only the waveform.
    for (int i = 0; i < 500 && (engine.waveform() != waveform || geiss::modeHasEdges(engine.mapMode())); ++i) {
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
    check(one.solarMax == 800, "solar strength comes from the truecolor table");
    check(at(geiss::modeEffects(8), Effect::Shade) == 210, "mode 8 keeps its chained shade weight of 60");
    const geiss::ModeEffects fifteen = geiss::modeEffects(15);
    check(fifteen.minEffects == 0 && fifteen.maxEffects == 1 && at(fifteen, Effect::Chasers) == 0, "mode 15 allows one effect");
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
    bool added = true;
    for (int mode = geiss::kFirstMapMode; mode <= geiss::kLastMapMode; ++mode) {
        const geiss::ModeEffects info = geiss::modeEffects(mode);
        added = added && at(info, Effect::Spectral) == 100 && at(info, Effect::Crystals) == 100;
    }
    check(added, "spectral dots and crystals have weight 100 on every map");
    check(geiss::modeHasEdges(6) && !geiss::modeHasEdges(5) && !geiss::modeHasEdges(25), "only mode 6 draws edges");
    check(std::string(geiss::effectName(Effect::Spectral)) == "Spectral dots" &&
              std::string(geiss::effectName(Effect::Crystals)) == "Crystals",
          "the new effects use their glossary names");
}

void testRandomEffectsRespectLimits() {
    using geiss::Effect;
    geiss::Random random{11};
    bool withinMax = true;
    bool withinMin = true;
    bool noBarWithGrid = true;
    bool hideNeedsNuclide = true;
    bool spectralHides = true;
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
            const bool nuclideHides = effects.has(Effect::Nuclide) && mode != 10;
            hideNeedsNuclide = hideNeedsNuclide && (!effects.hideWaveform || nuclideHides || effects.has(Effect::Spectral));
            spectralHides = spectralHides && (!effects.has(Effect::Spectral) || effects.hideWaveform);
            for (std::size_t e = 0; e < effects.on.size(); ++e) {
                zeroNeverComes = zeroNeverComes && !(effects.on[e] && info.weights[e] == 0);
            }
            chaserCount = chaserCount && (effects.chasers == 1 || effects.chasers == 2);
        }
    }
    check(withinMax, "a look never has more effects than its mode allows");
    check(withinMin, "without sound a look meets its mode's minimum");
    check(noBarWithGrid, "a grid turns the bar off");
    check(hideNeedsNuclide, "only the nuclide and spectral dots hide the waveform, the nuclide never over mode 10");
    check(spectralHides, "spectral dots always replace the waveform");
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


int litCount(const Pixels& pixels) {
    const std::vector<std::uint8_t> levels = pixels.levels();
    return static_cast<int>(std::count_if(levels.begin(), levels.end(), [](std::uint8_t p) { return p > 0; }));
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
    check(maxLevel(shade) <= 254, "shade bobs stop adding at 250");

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
    for (const std::uint8_t p : bar.levels()) {
        sixteens = sixteens && p % 16 == 0;
    }
    check(sixteens, "one frame of the bar adds 16 per point");
    check(litReach(bar, width, cx, cy) <= 31.f * scale * 1.5f + 2.f, "the bar stays near the center");
    const Pixels slowBar = runEffects(only(Effect::Bar), 25, width, height, 60, 1, sound);
    check(maxLevel(slowBar) <= 238, "the bar stops adding at 223");
    const Pixels fastBar = runEffects(only(Effect::Bar), 25, width, height, 1, 4, sound);
    check(maxLevel(fastBar) >= 12, "at 120 fps the bar adds its share each tick");

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

geiss::AudioFrame spectrumFrame(float bass, float middle, float treble) {
    geiss::AudioFrame audio = beatFrame(false, 1.f, 0.f);
    audio.spectrum = {bass, middle, treble};
    return audio;
}

// Whether some lit pixel has channels that differ by more than `spread`.
bool hasColor(const Pixels& pixels, int spread) {
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const geiss::Rgb c = pixels.rgb(i);
        if (std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}) > spread) {
            return true;
        }
    }
    return false;
}

void testTruecolorEffects() {
    using geiss::Effect;
    constexpr int width = 320;
    constexpr int height = 240;
    const geiss::AudioFrame sound = beatFrame(false, 1.f, 0.f);

    // The bar's channels are drawn apart in time, so over a run of frames its trail fringes.
    const Pixels bar = runEffects(only(Effect::Bar), 25, width, height, 40, 1, sound);
    check(hasColor(bar, 15), "the bar's channels land apart, fringing it in color");

    // Each dot is one color whose channels cycle at their own rates.
    const Pixels dot = runEffects(only(Effect::Dots), 25, width, height, 1, 1, sound);
    check(hasColor(dot, 10), "a dot is colored, not grey");
    const Pixels later = runEffects(only(Effect::Dots), 25, width, height, 40, 1, sound);
    std::set<std::array<int, 3>> colors;
    for (std::size_t i = 0; i < later.size(); ++i) {
        const geiss::Rgb c = later.rgb(i);
        if (later[i] > 0) {
            colors.insert({c.r, c.g, c.b});
        }
    }
    check(colors.size() > 1, "the dots' color changes over time");
    check(litCount(runEffects(only(Effect::Dots), 25, 880, 495, 1, 1, sound)) == 4, "from 880 wide a dot is 2x2");
    // From 1800 wide each grid dot is 2x2, so the grid has four times the lit pixels.
    const int narrowGrid = litCount(runEffects(only(Effect::Grid), 25, 1790, 1007, 1, 1, sound));
    const int wideGrid = litCount(runEffects(only(Effect::Grid), 25, 1800, 1013, 1, 1, sound));
    check(narrowGrid > 0 && wideGrid >= narrowGrid * 3, "from 1800 wide a grid dot is 2x2");

    // Shade bobs switch each channel on and off, so their glow takes on color.
    const Pixels shade = runEffects(only(Effect::Shade), 25, width, height, 200, 1, sound);
    check(hasColor(shade, 5), "shade bobs glow in drifting colors");
    check(maxLevel(shade) <= 254, "shade bobs stop adding at 250");

    // Each solar particle gives every channel its own strength.
    const Pixels solar = runEffects(only(Effect::Solar), 1, width, height, 10, 1, sound);
    check(hasColor(solar, 5), "solar particles sparkle in color");

    // The nuclide's rings take the drift's tint.
    Pixels rings(static_cast<std::size_t>(width) * height, 0);
    geiss::EffectLayer layer(5);
    geiss::ColorDrift drift;
    drift.offset = 1234;
    layer.setColorDrift(drift);
    geiss::AudioFrame jump = sound;
    jump.volume = 150.f;
    layer.draw(rings.data(), width, height, only(Effect::Nuclide), 25, width / 2.f, height / 2.f, 1.0, jump);
    check(litCount(rings) > 20 && hasColor(rings, 10), "nuclide rings are tinted");

    // Each spectral dot has a color whose channels add up to at least 1.2.
    const Pixels spectral = runEffects(only(Effect::Spectral), 25, 640, 360, 1, 1, spectrumFrame(255.f, 255.f, 255.f));
    check(hasColor(spectral, 10), "spectral dots are colored");

    // The grid and crystals raise every channel alike over black.
    const Pixels grid = runEffects(only(Effect::Grid), 25, width, height, 1, 1, sound);
    const Pixels crystal = runEffects(only(Effect::Crystals), 25, width, height, 1, 1, sound);
    check(!hasColor(grid, 0) && !hasColor(crystal, 0), "the grid and crystals draw grey");

    // The fixed textures are held to SDR: the grid's pulse, crystals, and mode 6's edges never
    // glow, while the chasers' white does.
    const auto neverGlows = [](const Pixels& p) {
        bool calm = true;
        for (std::size_t i = 0; i < p.size(); ++i) {
            calm = calm && (p[i] == 0 || (p.held(i) == p[i] && p.shownHeld(i) == 255));
        }
        return calm;
    };
    const Pixels edges = runEffects({}, 6, width, height, 1, 1, sound);
    check(litCount(grid) > 0 && neverGlows(grid), "the grid is held to SDR");
    check(litCount(crystal) > 0 && neverGlows(crystal), "crystals are held to SDR");
    check(litCount(edges) > 0 && neverGlows(edges), "mode 6's edges are held to SDR");
    bool edgesBright = false;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        edgesBright = edgesBright || edges[i] >= 208;
    }
    check(edgesBright, "mode 6's edges still reach full brightness");

    Pixels heldWhite(static_cast<std::size_t>(width) * height, 150);
    for (std::size_t i = 0; i < heldWhite.size(); ++i) {
        heldWhite.setHeld(i, 150);
    }
    geiss::EffectLayer chaserLayer(3);
    chaserLayer.draw(heldWhite.data(), width, height, only(Effect::Chasers), 25, width / 2.f, height / 2.f, 1.0, sound);
    bool chaserGlows = false;
    for (std::size_t i = 0; i < heldWhite.size(); ++i) {
        chaserGlows = chaserGlows || (heldWhite.held(i) == 0 && heldWhite.shownHeld(i) == 0 && heldWhite[i] > 150);
    }
    check(chaserGlows, "a chaser over a held texture takes it over and glows");

    // A texture under a brighter color leaves that color's glow alone.
    Pixels hot(static_cast<std::size_t>(width) * height, 220);
    geiss::EffectLayer hotLayer(4);
    hotLayer.draw(hot.data(), width, height, only(Effect::Crystals), 25, width / 2.f, height / 2.f, 1.0, sound);
    bool unheld = true;
    for (std::size_t i = 0; i < hot.size(); ++i) {
        unheld = unheld && hot.held(i) == 0;
    }
    check(unheld, "crystals under a brighter color do not hold it");
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
    pixels.clear();
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

    // Silence keeps the picture calm: no other effect draws, and a burst due in silence is dropped.
    geiss::EffectSet every;
    every.on.fill(true);
    every.set(Effect::Nuclide, false);
    every.chasers = 2;
    every.solarBurst = true;
    check(litCount(runEffects(every, 1, width, height, 120, 1, quietFrame())) == 0,
          "no effect but the nuclide draws in silence");
    check(litCount(runEffects(every, 6, width, height, 120, 1, quietFrame())) == 0, "mode 6's edges rest in silence");

    // In silence the nuclide drops a random ring about one frame in 12.
    int silentRings = 0;
    geiss::EffectLayer resting(9);
    for (int i = 0; i < 1200; ++i) {
        pixels.clear();
        resting.draw(pixels.data(), width, height, effects, 25, width / 2.f, height / 2.f, 1.0, quietFrame());
        if (litCount(pixels) > 0) {
            ++silentRings;
            const float ringReach = litReach(pixels, width, width / 2.f, height / 2.f);
            check(ringReach < (42.f + 11.f) * 0.5f + 2.f, "a silent ring sits around the center");
        }
    }
    check(silentRings > 60 && silentRings < 140, "the nuclide drops a ring about one frame in 12 in silence");
    int fastRings = 0;
    geiss::EffectLayer fastResting(9);
    for (int i = 0; i < 4800; ++i) {
        pixels.clear();
        fastResting.draw(pixels.data(), width, height, effects, 25, width / 2.f, height / 2.f, 0.25, quietFrame());
        fastRings += litCount(pixels) > 0 ? 1 : 0;
    }
    check(fastRings > 60 && fastRings < 140, "silent rings keep their pace at 120 fps");

    geiss::EffectLayer late(5);
    geiss::EffectSet burstOnly;
    burstOnly.solarBurst = true;
    late.startLook(burstOnly);
    pixels.clear();
    late.draw(pixels.data(), width, height, burstOnly, 1, width / 2.f, height / 2.f, 1.0, quietFrame());
    late.draw(pixels.data(), width, height, burstOnly, 1, width / 2.f, height / 2.f, 1.0, steady);
    check(litCount(pixels) == 0, "a look that opens in silence loses its solar burst");
}

void testSpectralDots() {
    using geiss::Effect;
    constexpr int width = 640;
    constexpr int height = 360;
    constexpr float cx = width / 2.f;
    constexpr float cy = height / 2.f;
    const geiss::EffectSet effects = only(Effect::Spectral);
    check(litCount(runEffects(effects, 25, width, height, 30, 1, spectrumFrame(0.f, 0.f, 0.f))) == 0,
          "spectral dots stay dark without band levels");
    check(litCount(runEffects(effects, 25, width, height, 30, 1, spectrumFrame(30.f, 30.f, 30.f))) == 0,
          "quiet bands draw no spectral dots");
    const Pixels loud = runEffects(effects, 25, width, height, 1, 1, spectrumFrame(255.f, 255.f, 255.f));
    check(litCount(loud) > 0, "loud bands draw spectral dots");
    check(litReach(loud, width, cx, cy) < 115.f + 96.f + 12.f, "spectral dots stay on their paths around the center");
    // After one frame the smoothed level is 0.6 of 200, a radius of 5.2: up to 3 discs of 5.
    const Pixels one = runEffects(effects, 25, width, height, 1, 1, spectrumFrame(200.f, 0.f, 0.f));
    check(litCount(one) > 50 && litCount(one) < 120, "one band draws one disc sized by its level");
    // Four display frames at 120 fps leave the levels where one frame at 30 fps does.
    auto nextFrame = [&](int ticks) {
        geiss::EffectLayer layer(3);
        layer.startLook(effects);
        Pixels pixels(static_cast<std::size_t>(width) * height, 0);
        for (int i = 0; i < ticks; ++i) {
            layer.draw(pixels.data(), width, height, effects, 25, cx, cy, 1.0 / ticks, spectrumFrame(200.f, 0.f, 0.f));
        }
        pixels.clear();
        layer.draw(pixels.data(), width, height, effects, 25, cx, cy, 1.0, spectrumFrame(200.f, 0.f, 0.f));
        return pixels;
    };
    check(nextFrame(4) == nextFrame(1), "spectral dots update once per reference frame at 120 fps");
    auto sum = [](const Pixels& pixels) {
        long total = 0;
        for (const std::uint8_t p : pixels.levels()) {
            total += p;
        }
        return total;
    };
    // A display frame at 120 fps adds about a quarter of a reference frame's light.
    auto settledStamp = [&](double frames) {
        geiss::EffectLayer layer(3);
        layer.startLook(effects);
        Pixels pixels(static_cast<std::size_t>(width) * height, 0);
        for (int i = 0; i < 30; ++i) {
            layer.draw(pixels.data(), width, height, effects, 25, cx, cy, 1.0, spectrumFrame(200.f, 0.f, 0.f));
        }
        pixels.clear();
        layer.draw(pixels.data(), width, height, effects, 25, cx, cy, frames, spectrumFrame(200.f, 0.f, 0.f));
        return sum(pixels);
    };
    const long full = settledStamp(1.0);
    const long quarter = settledStamp(0.25);
    check(full > 0 && quarter > full * 15 / 100 && quarter < full * 30 / 100,
          "at 120 fps a display frame stamps the spectral dots with its share of a frame");

    // At 120 fps every display frame stamps the discs once they have a level.
    {
        geiss::EffectLayer layer(3);
        layer.startLook(effects);
        Pixels pixels(static_cast<std::size_t>(width) * height, 0);
        bool everyFrame = true;
        for (int i = 0; i < 16; ++i) {
            pixels.clear();
            layer.draw(pixels.data(), width, height, effects, 25, cx, cy, 0.25, spectrumFrame(200.f, 200.f, 200.f));
            if (i >= 3) {
                everyFrame = everyFrame && litCount(pixels) > 0;
            }
        }
        check(everyFrame, "at 120 fps spectral dots draw on every display frame");
    }

    // A slow fade within 4 levels moves the disc forward, as in the original; a steep fall holds it.
    auto discCenter = [&](const Pixels& pixels) {
        double x = 0;
        double y = 0;
        double weight = 0;
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const int column = static_cast<int>(i) % width;
            const int row = static_cast<int>(i) / width;
            x += static_cast<double>(pixels[i]) * static_cast<double>(column);
            y += static_cast<double>(pixels[i]) * static_cast<double>(row);
            weight += pixels[i];
        }
        return std::pair{x / weight, y / weight};
    };
    auto centerAfter = [&](float second) {
        geiss::EffectLayer layer(3);
        layer.startLook(effects);
        Pixels pixels(static_cast<std::size_t>(width) * height, 0);
        for (int i = 0; i < 30; ++i) {
            layer.draw(pixels.data(), width, height, effects, 25, cx, cy, 1.0, spectrumFrame(200.f, 0.f, 0.f));
        }
        pixels.clear();
        layer.draw(pixels.data(), width, height, effects, 25, cx, cy, 1.0, spectrumFrame(200.f, 0.f, 0.f));
        const auto before = discCenter(pixels);
        pixels.clear();
        layer.draw(pixels.data(), width, height, effects, 25, cx, cy, 1.0, spectrumFrame(second, 0.f, 0.f));
        const auto after = discCenter(pixels);
        return std::hypot(after.first - before.first, after.second - before.second);
    };
    check(centerAfter(200.f) < 0.5, "a steady band holds its disc still");
    check(centerAfter(195.f) > 0.5, "a slight fall moves the disc a little, as in the original");
    check(centerAfter(150.f) < 0.5, "a steep fall holds the disc where it is");

    // A new look's spectral dots start from its own sound, not the last look's.
    {
        geiss::EffectLayer layer(3);
        layer.startLook(effects);
        Pixels pixels(static_cast<std::size_t>(width) * height, 0);
        for (int i = 0; i < 30; ++i) {
            layer.draw(pixels.data(), width, height, effects, 25, cx, cy, 1.0, spectrumFrame(255.f, 255.f, 255.f));
        }
        layer.startLook(only(Effect::Chasers));
        layer.startLook(effects);
        pixels.clear();
        layer.draw(pixels.data(), width, height, effects, 25, cx, cy, 1.0, spectrumFrame(0.f, 0.f, 0.f));
        check(litCount(pixels) == 0, "a new look's spectral dots forget the last look's levels");
    }
}

void testCrystalsAndEdges() {
    using geiss::Effect;
    constexpr int width = 320;
    constexpr int height = 240;
    constexpr float scale = width / 640.f;
    const geiss::AudioFrame sound = beatFrame(false, 1.f, 0.f);

    const Pixels crystal = runEffects(only(Effect::Crystals), 25, width, height, 1, 1, sound);
    bool floor150 = litCount(crystal) > 0;
    for (const std::uint8_t p : crystal.levels()) {
        floor150 = floor150 && (p == 0 || p == 150);
    }
    check(floor150, "crystal spokes raise their pixels to 150");
    check(litReach(crystal, width, width / 2.f, height / 2.f) < (21.3f + 25.f) * scale + 2.f,
          "crystals burst near the center");
    Pixels bright(static_cast<std::size_t>(width) * height, 200);
    geiss::EffectLayer crystalLayer(4);
    crystalLayer.draw(bright.data(), width, height, only(Effect::Crystals), 25, width / 2.f, height / 2.f, 1.0, sound);
    check(allLevels(bright, [](std::uint8_t p) { return p == 200; }),
          "crystals never darken a pixel");

    const Pixels edges = runEffects({}, 6, width, height, 1, 1, sound);
    bool onEdges = true;
    int lit = 0;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        const int y = static_cast<int>(i) / width;
        const bool edgeRow = y < 3 || y >= height - 3;
        // Each channel's wave reaches 198, plus noise of up to 39.
        onEdges = onEdges && (edgeRow ? edges[i] < 238 : edges[i] == 0);
        lit += edgeRow && edges[i] > 0 ? 1 : 0;
    }
    check(onEdges, "mode 6 draws noise on three rows at the top and bottom");
    // The channels follow their own waves, so along a row the color changes.
    bool tinted = false;
    for (int x = 0; x < width && !tinted; ++x) {
        const geiss::Rgb c = edges.rgb(static_cast<std::size_t>(x));
        tinted = std::abs(c.r - c.g) > 60 || std::abs(c.g - c.b) > 60 || std::abs(c.r - c.b) > 60;
    }
    check(tinted, "mode 6's edges are colored per channel");
    check(lit > width * 3, "mode 6's edges cover the rows");
    check(litCount(runEffects({}, 25, width, height, 1, 1, sound)) == 0, "other maps draw no edges");
    // The edges are rewritten once per reference frame, so 120 fps uses the same noise as 30 fps.
    {
        geiss::EffectLayer layer(4);
        Pixels pixels(static_cast<std::size_t>(width) * height, 0);
        int stamps = 0;
        for (int i = 0; i < 8; ++i) {
            pixels.clear();
            layer.draw(pixels.data(), width, height, {}, 6, width / 2.f, height / 2.f, 0.25, sound);
            stamps += litCount(pixels) > 0 ? 1 : 0;
        }
        check(stamps == 2, "at 120 fps mode 6 rewrites its edges once per reference frame");
    }
}

void testSpectrumBands() {
    constexpr double rate = 48000.0;
    constexpr int frames = 4800;
    auto bandsOf = [&](double hz, float amplitude) {
        std::vector<float> tone(frames);
        for (int i = 0; i < frames; ++i) {
            tone[static_cast<std::size_t>(i)] =
                amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * i / rate));
        }
        geiss::Analyzer analyzer;
        geiss::PcmBlock block;
        block.samples = tone.data();
        block.frameCount = frames;
        block.channels = 1;
        block.sampleRate = rate;
        return analyzer.analyze(block, 64, 1.f).spectrum;
    };
    const auto bass = bandsOf(60.0, 0.1f);
    check(bass[0] > bass[1] && bass[0] > bass[2], "a 60 Hz tone fills the bass band");
    const auto middle = bandsOf(800.0, 0.1f);
    check(middle[1] > middle[0] && middle[1] > middle[2], "an 800 Hz tone fills the middle band");
    const auto treble = bandsOf(8000.0, 0.1f);
    check(treble[2] > treble[0] && treble[2] > treble[1], "an 8 kHz tone fills the treble band");
    const auto quiet = bandsOf(800.0, 0.f);
    check(quiet[0] == 0.f && quiet[1] == 0.f && quiet[2] == 0.f, "silence has no band levels");
    const auto loud = bandsOf(60.0, 1.f);
    check(loud[0] == 255.f, "band levels stop at 255");
}

void testFourierPower() {
    checkNear(static_cast<float>(geiss::fourierBandHz(12)), 640.f, 0.01f, "band 12 is centered on 640 Hz");
    checkNear(static_cast<float>(geiss::fourierBandHz(24)), 20480.f, 0.1f, "ten octaves up from 20 Hz at band 24");
    constexpr double rate = 48000.0;
    for (const int band : {12, 15, 18, 22}) {
        // 256 samples at 48 kHz resolve about 190 Hz, so the bass bands below band 12 blur together.
        const auto tone = toneSamples(geiss::fourierBandHz(band), 0.5f, rate, 256);
        const auto power = geiss::fourierPower(tone.data(), 256, rate);
        check(strongestBand(power) == static_cast<std::size_t>(band), "a tone at a band's center peaks in that band");
        check(power[0] == 0.f, "band 0 is never measured");
    }
    const std::vector<float> flat(256, 0.3f);
    const auto dc = geiss::fourierPower(flat.data(), 256, rate);
    check(std::all_of(dc.begin(), dc.end(), [](float p) { return p < 1e-3f; }), "a constant offset has no power");
    const auto none = geiss::fourierPower(nullptr, 256, rate);
    check(std::all_of(none.begin(), none.end(), [](float p) { return p == 0.f; }), "no samples, no power");

    // The window keeps the original's 256 samples at 44.1 kHz in time at other rates.
    check(geiss::fourierWindow(44100.0) == 256, "the colour spectrum reads 256 samples at 44.1 kHz");
    check(geiss::fourierWindow(48000.0) == 279, "the colour spectrum reads 279 samples at 48 kHz");
    check(geiss::fourierWindow(96000.0) == 557, "the colour spectrum reads 557 samples at 96 kHz");
    check(geiss::fourierWindow(0.0) == 256, "an unknown rate reads the original's 256 samples");
}

void testSmoothedPower() {
    constexpr double rate = 48000.0;
    const auto tone = toneSamples(geiss::fourierBandHz(18), 0.5f, rate, 4800);
    // The same half second of the same tone at 30 and at 120 blocks per second.
    auto smoothedAfter = [&](int blocksPerSecond) {
        geiss::Analyzer analyzer;
        geiss::AudioFrame frame;
        for (int i = 0; i <= blocksPerSecond / 2; ++i) {
            geiss::PcmBlock block;
            block.samples = tone.data();
            block.frameCount = 4800;
            block.channels = 1;
            block.sampleRate = rate;
            block.time = static_cast<double>(i) / blocksPerSecond;
            frame = analyzer.analyze(block, 64, 1.f);
        }
        return frame.power;
    };
    const auto slow = smoothedAfter(30);
    const auto fast = smoothedAfter(120);
    checkNear(fast[18] / slow[18], 1.f, 0.05f, "smoothing follows time, not the block rate");
    const auto direct = geiss::fourierPower(tone.data(), 256, rate);
    check(slow[18] < direct[18] && slow[18] > direct[18] * 0.5f, "half a second rises most of the way");
    check(slow[0] == 0.f, "band 0 stays 0");

    geiss::Analyzer quiet;
    const std::vector<float> zeros(4800, 0.f);
    geiss::PcmBlock block;
    block.samples = zeros.data();
    block.frameCount = 4800;
    block.channels = 1;
    block.sampleRate = rate;
    const auto silent = quiet.analyze(block, 64, 1.f).power;
    check(std::all_of(silent.begin(), silent.end(), [](float p) { return p == 0.f; }), "silence has no colour power");
}

void testWaveColor() {
    constexpr double rate = 48000.0;
    const geiss::ColorDrift drift;
    auto colorOf = [&](double hz, float base) {
        const auto tone = toneSamples(hz, 0.5f, rate, 256);
        return geiss::waveColor(base, 100.0, drift, geiss::fourierPower(tone.data(), 256, rate));
    };
    // 256 samples are about 5 ms, a fraction of a bass cycle, so the bass colour depends on the
    // phase the window catches, as in the original. Deep bass reads red.
    const geiss::Rgb bass = colorOf(40.0, 100.f);
    check(bass.r > bass.g && bass.r > bass.b, "bass draws red");
    const geiss::Rgb middle = colorOf(geiss::fourierBandHz(11), 100.f);
    check(middle.g > middle.r && middle.g > middle.b, "the middle draws green");
    const geiss::Rgb treble = colorOf(8000.0, 100.f);
    check(treble.b > treble.r && treble.b > treble.g, "treble draws blue");

    // Below the clamp the color's length is about base * 1.35, plus the drift's small share.
    const geiss::Rgb dim = colorOf(geiss::fourierBandHz(11), 40.f);
    const float length = std::sqrt(static_cast<float>(dim.r * dim.r + dim.g * dim.g + dim.b * dim.b));
    checkNear(length, 40.f * 1.35f, 6.f, "the spectrum color is scaled to the brightness");

    const std::array<float, geiss::kFourierDetail> none{};
    check(geiss::waveColor(100.f, 100.0, drift, none) == geiss::driftColor(100.f, 100.0, drift),
          "without spectrum power the wave takes the drift color");
    for (int frame = 0; frame < 5000; frame += 37) {
        for (const geiss::Rgb c : {geiss::driftColor(155.f, frame, drift), colorOf(40.0, 155.f), colorOf(8000.0, 155.f)}) {
            check(c.r >= 0 && c.r <= 255 && c.g >= 0 && c.g <= 255 && c.b >= 0 && c.b <= 255, "colors stay in 0...255");
        }
    }
    check(geiss::driftColor(0.f, 10.0, drift) == geiss::Rgb{}, "no brightness, no color");
}

void testColorDrift() {
    geiss::Random random;
    random.state = 7;
    const geiss::ColorDrift drift = geiss::randomColorDrift(random);
    for (const float rate : drift.rates) {
        check(rate >= 0.02f && rate < 0.03f, "drift rates are 0.02...0.03");
    }
    check(drift.offset >= 0 && drift.offset < 40000, "the drift phase is 0...39999");
    // The drift moves: over a few hundred frames each channel changes.
    const geiss::Rgb early = geiss::driftColor(100.f, 0.0, drift);
    const geiss::Rgb later = geiss::driftColor(100.f, 300.0, drift);
    check(!(early == later), "the drift color changes over time");

    float lowest = 1.f;
    float highest = 0.f;
    float silentLowest = 1.f;
    float silentHighest = 0.f;
    for (int frame = 0; frame < 20000; frame += 3) {
        const geiss::Tint tint = geiss::nuclideTint(frame, drift);
        const geiss::Tint silent = geiss::silentNuclideTint(frame, drift);
        for (const float c : {tint.r, tint.g, tint.b}) {
            lowest = std::min(lowest, c);
            highest = std::max(highest, c);
        }
        for (const float c : {silent.r, silent.g, silent.b}) {
            silentLowest = std::min(silentLowest, c);
            silentHighest = std::max(silentHighest, c);
        }
    }
    check(lowest >= 0.159f && highest <= 1.001f && highest - lowest > 0.5f, "the nuclide tint swings within 0.16...1");
    check(silentLowest >= -0.001f && silentHighest <= 1.001f && silentHighest - silentLowest > 0.6f,
          "the silent nuclide tint swings within 0...1");
    const geiss::Tint ring = geiss::silentNuclideTint(500.0, drift);
    const geiss::Tint swapped = geiss::nuclideTint(500.0, drift);
    check(std::fabs((ring.r - 0.5f) / 0.25f - (swapped.b - 0.58f) / 0.21f) < 1e-3f,
          "the silent ring's red follows the blue phase, as the original's swapped bytes did");
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
    const Pixels zoom = filled(25);
    check(allLevels(zoom, [](std::uint8_t p) { return p == 200; }), "mode 25 leaves the center alone");
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
        if (!geiss::isMapMode(mode)) {
            continue;
        }
        const std::string name = geiss::mapName(mode);
        check(!name.empty() && name != "?", "every map mode has a name");
        maps.insert(name);
    }
    check(maps.size() == static_cast<std::size_t>(geiss::kLastMapMode - 1), "map names are unique");
    check(std::string(geiss::mapName(12)) == "?", "mode 12 has no name");
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
    // The fade itself, without the idle wave that rises over it.
    engine.setIdleWave(false);
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

// The brightest channel in the frame, which picks the top palette entry it shows.
unsigned maxChannel(const geiss::FrameView& frame) {
    unsigned highest = 0;
    const std::size_t pixels = static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    for (std::size_t i = 0; i < pixels; ++i) {
        highest = std::max<unsigned>({highest, frame.rgba[i * 4], frame.rgba[i * 4 + 1], frame.rgba[i * 4 + 2]});
    }
    return highest;
}

// Ticks `seconds` of silence at `fps` and returns the last frame.
geiss::FrameView tickSilence(geiss::Engine& engine, double seconds, double fps) {
    geiss::FrameView frame{};
    for (int i = 0; i < static_cast<int>(seconds * fps); ++i) {
        frame = engine.tick(1.0 / fps, quietFrame());
    }
    return frame;
}

// The circle starts its smoother from the blended first point, as the original did after
// blending in place, so an odd first sample does not dent the ring at the seam.
void testCircleSeam() {
    constexpr int width = 160;
    constexpr int height = 90;
    geiss::Engine engine = engineDrawing(5, width, height);
    geiss::AudioFrame audio = flatWave(width);
    std::fill(audio.samples.begin(), audio.samples.end(), 0.5f);
    audio.samples[0] = -1.f;
    const geiss::FrameView frame = engine.tick(1.0 / 30.0, audio);
    const float centerX = engine.map().centerXNorm * static_cast<float>(width);
    const float centerY = engine.map().centerYNorm * static_cast<float>(height);
    // Base radius 15 at 160 wide, plus half of the 22-pixel amplitude.
    const float ring = 15.f + 0.5f * static_cast<float>(height) * 0.35f * 0.7f;
    float nearest = 1e9f;
    for (const auto& [x, y] : litPixels(frame)) {
        nearest = std::min(nearest, std::hypot(static_cast<float>(x) - centerX, static_cast<float>(y) - centerY));
    }
    check(nearest > ring - 3.f, "the circle has no notch at its seam");
}

void testSoftSpot() {
    constexpr int size = 9;
    constexpr auto side = static_cast<std::size_t>(size);
    Pixels pixels(side * side, 0);
    auto at = [&](int x, int y) { return pixels[(static_cast<std::size_t>(y) * side) + static_cast<std::size_t>(x)]; };
    geiss::plotSoftSpot(pixels.data(), size, size, 4, 4, {200, 200, 200});
    check(at(4, 4) == 200, "a soft spot's center is the full color");
    check(at(3, 4) == at(5, 4) && at(4, 3) == at(4, 5), "a soft spot is symmetric");
    check(at(5, 4) > 100 && at(5, 4) < 200, "a soft spot's neighbors are dimmer");
    check(at(6, 6) > 0 && at(6, 6) < at(5, 5), "a soft spot fades toward its corners");
    check(at(7, 4) == 0 && at(4, 1) == 0, "a soft spot spans 5x5");
    geiss::plotSoftSpot(pixels.data(), size, size, 4, 4, {50, 50, 50});
    check(at(4, 4) == 200, "a soft spot keeps a brighter pixel");
    geiss::plotSoftSpot(pixels.data(), size, size, 0, 0, {200, 200, 200});
    check(at(0, 0) == 200 && at(1, 0) > 100, "a soft spot clips at the edges");
}

void testIdleWave() {
    geiss::Engine engine = engineDrawing(1, 160, 90);
    engine.setLocked(true);
    tickSilence(engine, 1.4, 30);
    check(engine.idleLevel() == 0.f, "the idle wave waits through the first 1.4 s of silence");
    tickSilence(engine, 0.2, 30);
    check(engine.idleLevel() > 0.f && engine.idleLevel() < 0.05f, "the idle wave starts rising after 1.5 s of silence");
    const geiss::FrameView settled = tickSilence(engine, 8.0, 30);
    check(engine.idleLevel() == 1.f, "the idle wave has risen after about six seconds of silence");
    check(litPixels(settled).size() > 160, "silence keeps a moving picture");
    // The drift swings each channel up to 2.5 times the idle brightness; even that stays in the
    // dim half of the palette.
    check(maxChannel(settled) < 128, "the idle wave stays dim");

    const std::size_t bytes = static_cast<std::size_t>(settled.width) * static_cast<std::size_t>(settled.height) * 4;
    const std::vector<std::uint8_t> before(settled.rgba, settled.rgba + bytes);
    const geiss::FrameView later = tickSilence(engine, 2.0, 30);
    const std::vector<std::uint8_t> after(later.rgba, later.rgba + bytes);
    check(before != after, "the idle wave moves");

    // Loud sound fades the idle wave out over half a second instead of cutting it.
    engine.tick(1.0 / 30.0, beatFrame(false, 1.f, 0.f));
    check(engine.idleLevel() > 0.9f && engine.idleLevel() < 1.f, "sound starts fading the idle wave out");
    for (int i = 0; i < 6; ++i) {
        engine.tick(1.0 / 30.0, beatFrame(false, 1.f, 0.f));
    }
    const float fading = engine.idleLevel();
    check(fading > 0.f && fading < 0.6f, "the idle wave is partway out after a quarter second of sound");
    // Quiet returning mid-fade brings it back up at once, without the wait.
    tickSilence(engine, 0.1, 30);
    check(engine.idleLevel() > fading, "quiet during the fade brings the idle wave back without a wait");
    for (int i = 0; i < 20; ++i) {
        engine.tick(1.0 / 30.0, beatFrame(false, 1.f, 0.f));
    }
    check(engine.idleLevel() == 0.f, "sound has replaced the idle wave after half a second");
    tickSilence(engine, 1.4, 30);
    check(engine.idleLevel() == 0.f, "once gone, the idle wave waits again");

    geiss::Engine paused = engineDrawing(1, 160, 90);
    tickSilence(paused, 3.0, 30);
    const float held = paused.idleLevel();
    paused.setPaused(true);
    tickSilence(paused, 2.0, 30);
    check(paused.idleLevel() == held, "pause holds the idle wave");

    geiss::Engine off = engineDrawing(1, 160, 90);
    off.setIdleWave(false);
    check(litPixels(tickSilence(off, 10.0, 30)).empty(), "with the idle wave off, silence fades to black");

    // A nuclide look hides its waveform, and its rings rest in silence, so the idle wave
    // draws the circle instead of leaving the picture dark.
    geiss::Engine nuclide = engineDrawing(1, 160, 90);
    nuclide.setLocked(true);
    nuclide.setEffects(only(geiss::Effect::Nuclide));
    check(!litPixels(tickSilence(nuclide, 8.0, 30)).empty(), "a nuclide look keeps an idle wave");

    geiss::Engine slow = engineDrawing(1, 160, 90);
    geiss::Engine fast = engineDrawing(1, 160, 90);
    tickSilence(slow, 3.0, 30);
    tickSilence(fast, 3.0, 120);
    checkNear(slow.idleLevel(), fast.idleLevel(), 0.01f, "the idle wave rises at the same pace at any frame rate");
    // At 12.5 fps a tick that crosses the wait only counts the part past it: 18 ticks reach
    // 1.44 s, and the next ends 0.02 s past the wait.
    geiss::Engine coarse = engineDrawing(1, 160, 90);
    for (int i = 0; i < 19; ++i) {
        coarse.tick(0.08, quietFrame());
    }
    checkNear(coarse.idleLevel(), 0.02f / 4.f, 0.002f, "a tick across the idle wait counts only the time past it");

    // Sound too faint for the waveform to draw brings the idle wave up as silence does.
    geiss::AudioFrame faint = beatFrame(false, 1.f, 0.f);
    faint.volume = 1.f;
    faint.averageVolume = 1.f;
    geiss::Engine hushed = engineDrawing(1, 160, 90);
    hushed.setLocked(true);
    for (int i = 0; i < 9 * 30; ++i) {
        hushed.tick(1.0 / 30.0, faint);
    }
    check(hushed.soundActive(), "faint sound still counts as sound for the effects");
    check(hushed.idleLevel() == 1.f, "faint sound that draws no waveform brings up the idle wave");

    // Quiet sound that draws a line dimmer than the idle wave's darkest keeps the idle wave,
    // so quiet music never looks darker than silence. Volume 8 is a brightness of about 25.
    geiss::AudioFrame quiet = beatFrame(false, 1.f, 0.f);
    quiet.volume = 8.f;
    quiet.averageVolume = 8.f;
    for (int i = 0; i < 30; ++i) {
        hushed.tick(1.0 / 30.0, quiet);
    }
    check(hushed.idleLevel() == 1.f, "quiet sound dimmer than the idle wave keeps it");
    for (int i = 0; i < 30; ++i) {
        hushed.tick(1.0 / 30.0, beatFrame(false, 1.f, 0.f));
    }
    check(hushed.idleLevel() == 0.f, "a line brighter than the idle wave replaces it");

    // Beat mode's dark gaps between hits do not count as faint.
    geiss::Engine beating = engineDrawing(1, 160, 90);
    for (int i = 0; i < 3 * 30; ++i) {
        beating.tick(1.0 / 30.0, beatFrame(true, 0.f, 0.f));
    }
    check(beating.idleLevel() == 0.f, "beat mode's dark gaps keep the idle wave down");

    // The hand-off compares drawn colors: the line's brightest channel against the drift
    // color's average brightest channel at the idle wave's darkest base, about 26 * 1.58 = 41.
    // A bass line at brightness 34 is above that base but draws in a red of about 45, so it
    // replaces the idle wave, and a line at 28 draws dimmer. Neither flips as the drift moves.
    std::array<float, geiss::kFourierDetail> bass{};
    bass[3] = 1.f;
    const geiss::ColorDrift drift{};
    bool steady = true;
    for (int frame = 0; frame < 4000; frame += 7) {
        const auto t = static_cast<double>(frame);
        const geiss::Rgb line = geiss::waveColor(34.f, t, drift, bass);
        const geiss::Rgb faint = geiss::waveColor(28.f, t, drift, bass);
        check(geiss::drawsDimmerThanIdle(34.f, t, drift, bass) == (std::max({line.r, line.g, line.b}) < 41),
              "the idle hand-off follows the line's brightest drawn channel");
        steady = steady && !geiss::drawsDimmerThanIdle(34.f, t, drift, bass) &&
                 geiss::drawsDimmerThanIdle(28.f, t, drift, bass) && std::max({faint.r, faint.g, faint.b}) < 41;
    }
    check(steady, "a steady line stays on one side of the hand-off while the drift moves");
    // Without spectrum power the line is drift-colored like the idle wave, so only the base counts.
    const std::array<float, geiss::kFourierDetail> none{};
    bool byBase = true;
    for (int frame = 0; frame < 4000; frame += 7) {
        byBase = byBase && geiss::drawsDimmerThanIdle(25.f, frame, drift, none) &&
                 !geiss::drawsDimmerThanIdle(27.f, frame, drift, none);
    }
    check(byBase, "a line without spectrum power compares by brightness alone");
    check(!geiss::drawsDimmerThanIdle(155.f, 0.0, drift, bass), "a loud line always replaces the idle wave");
    check(geiss::drawsDimmerThanIdle(0.f, 0.0, drift, bass), "a dark line never replaces the idle wave");

}

// The idle wave's phases wrap in double, so they still advance one 120 Hz frame at a time
// after a week of running, when a float clock would only change every few frames.
void testWrappedPhase() {
    constexpr double week = 7.0 * 24.0 * 3600.0;
    const float now = geiss::wrappedPhase(0.31, week);
    const float next = geiss::wrappedPhase(0.31, week + 1.0 / 120.0);
    checkNear(next - now, 0.31f / 120.f, 0.0005f, "an idle phase advances smoothly after a week");
    check(std::fabs(now) < 6.2832f, "an idle phase wraps to one turn");
    check(geiss::wrappedPhase(-0.17, 100.0) <= 0.f, "a backward idle phase stays negative");
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
    testDisplayPixel();
    testPalettes();
    testPaletteChanges();
    testWaveFollowsTheSpectrum();
    testFadeMatchesAtAnyFrameRate();
    testSilenceFadesAtAnyFrameRate();
    testEngineSlideShift();
    testEveryModeBuildsAFiniteMap();
    testModeDamping();
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
    testTruecolorEffects();
    testSpectralDots();
    testCrystalsAndEdges();
    testSpectrumBands();
    testFourierPower();
    testSmoothedPower();
    testWaveColor();
    testColorDrift();
    testCenterDwindle();
    testMode5FollowsNuclide();
    testEngineEffects();
    testNames();
    testSoundActive();
    testCircleSeam();
    testSoftSpot();
    testIdleWave();
    testWrappedPhase();
    testWrapColumn();
    testBandsMatchSerial();
    testFrameChanged();
    testWarpRunner();
    testPipelinedWarp();
    testSharedBuffers();
    testHostExpands();
    if (failures != 0) {
        std::cerr << failures << " failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "ok\n";
    return EXIT_SUCCESS;
}
