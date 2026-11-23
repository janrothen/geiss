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

import AppKit
import QuartzCore
import SwiftUI

// What the debug info readout shows. Refreshed twice a second while it is visible.
struct InfoSnapshot: Equatable {
    var fps: Int
    var map: String
    var wave: String
    var effects: String
    var palette: String
    var hdr: String
    var simulation: String
}

enum PreviewSource: String {
    case system
    case tone
    case silence
}

@MainActor
final class VisualizerModel: ObservableObject {
    static let shared = VisualizerModel()

    let presenter = MetalPresenter()
    @Published var paused = false
    @Published var locked = false
    @Published var showAbout = false
    @Published var sensitivity: Double {
        didSet {
            let clamped = min(4, max(0.25, sensitivity))
            if clamped != sensitivity {
                sensitivity = clamped
                return
            }
            UserDefaults.standard.set(clamped, forKey: Self.sensitivityKey)
            bridge.setSensitivity(Float(clamped))
        }
    }
    @Published var preview: PreviewSource = .system
    @Published var capture: CaptureStatus = .starting
    // The debug readout of FPS, map, waveform, and effects. Not persisted.
    @Published private(set) var showInfo = false
    @Published private(set) var info: InfoSnapshot?

    private let bridge = GeissBridge(seed: 1)
    private let tap = SystemAudioTap()
    /// Frame times are display-link target times, on the `CACurrentMediaTime` clock.
    private var lastTime: CFTimeInterval?
    private var lastSequence: UInt64 = 0
    private var lastCaptureTime: CFTimeInterval?
    private var simSize: SimulationSize?
    /// A new size waits here until the view has held it for `resizeSettleSeconds`.
    private var pendingSize: SimulationSize?
    private var pendingSince: CFTimeInterval = 0
    /// When the first size applied. Sizes apply at once for `resizeSettleSeconds` after it.
    private var openedAt: CFTimeInterval?
    /// Room for the longest tick, so silence never reallocates.
    private var silenceBuffer = [Float](repeating: 0, count: VisualizerModel.maxSilenceFrames * 2)
    private var captureBuffer: [Float] = []
    private var fpsWindowStart: CFTimeInterval?
    private var fpsWindowFrames = 0
    private var fps = 0

    private static let infoInterval = 0.5
    /// A live resize or a fullscreen animation changes the size every frame. Rebuilding the
    /// simulation each time costs a map build and new textures, so a size applies once the view
    /// has held it this long. Until then the presenter stretches the current picture.
    private static let resizeSettleSeconds = 0.15
    private static let maxDelta = 0.1
    private static let maxSilenceFrames = Int(PreviewTone.sampleRate * maxDelta) + 1

    private static let sensitivityKey = "sensitivity"
    /// Debug launch arguments: `-GeissCPUWarp YES` keeps the warp and the expansion on the CPU,
    /// at the CPU path's size cap. `-GeissWarpCheck YES` compares every 120th GPU warp with the
    /// CPU's and prints the difference. `-GeissPaletteGain 0.8` scales how far up the palette
    /// a pixel's brightness reaches, for judging that mapping by eye.
    private static let cpuWarpKey = "GeissCPUWarp"
    private static let warpCheckKey = "GeissWarpCheck"
    private static let paletteGainKey = "GeissPaletteGain"
    /// The CPU warp and expansion keep 120 Hz up to about this longest side.
    private static let cpuLongestSide: CGFloat = 960

    var sourceLabel: String {
        switch preview {
        case .tone:
            return "TONE"
        case .silence:
            return "QUIET"
        case .system:
            switch capture {
            case .running:
                return "LIVE"
            case .silenced:
                return "NO AUDIO"
            case .starting, .failed:
                return "WAIT"
            }
        }
    }

    private init() {
        let stored = UserDefaults.standard.object(forKey: Self.sensitivityKey) as? Double ?? 1
        sensitivity = min(4, max(0.25, stored))
        bridge.setSensitivity(Float(sensitivity))
        let defaults = UserDefaults.standard
        // Reads a string from the launch arguments as a number too; 0 when it is missing.
        let paletteGain = defaults.double(forKey: Self.paletteGainKey)
        if paletteGain > 0 {
            presenter.paletteGain = Float(paletteGain)
        }
        if !defaults.bool(forKey: Self.cpuWarpKey) {
            _ = bridge.useGPU(
                with: presenter.library,
                queue: presenter.queue,
                check: defaults.bool(forKey: Self.warpCheckKey)
            )
        }
        // The tap delivers statuses on the main queue, in order.
        tap.onStatus = { [weak self] status in
            MainActor.assumeIsolated {
                self?.capture = status
            }
        }
        tap.activate()
    }

    func openCaptureSettings() {
        let candidates = [
            "x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?Privacy_AudioCapture",
            "x-apple.systempreferences:com.apple.preference.security?Privacy_AudioCapture"
        ]
        for candidate in candidates {
            guard let url = URL(string: candidate) else { continue }
            if NSWorkspace.shared.open(url) {
                return
            }
        }
    }

    func togglePaused() {
        paused.toggle()
        bridge.setPaused(paused)
    }

    func toggleLocked() {
        locked.toggle()
        bridge.setLocked(locked)
    }

    func nextVisual() {
        bridge.nextVisual()
        refreshInfo()
    }

    func toggleInfo() {
        showInfo.toggle()
        refreshInfo()
    }

    /// `time` is when this frame will be shown, on the `CACurrentMediaTime` clock.
    func tick(at time: CFTimeInterval, viewSize: CGSize) {
        // A view that has not been laid out yet has no size worth building a simulation for.
        if viewSize.width >= 1, viewSize.height >= 1 {
            applySize(simulationSize(for: viewSize), at: time)
        }

        let dt = cappedDelta(to: time)
        switch preview {
        case .tone:
            let samples = PreviewTone.samples(endingAt: time)
            samples.withUnsafeBufferPointer { buffer in
                bridge.ingestInterleaved(
                    buffer.baseAddress,
                    frameCount: Int32(samples.count / 2),
                    channels: 2,
                    sampleRate: PreviewTone.sampleRate,
                    time: time
                )
            }
        case .silence:
            ingestSilence(dt: dt, time: time)
        case .system:
            ingestSystem(dt: dt, time: time)
        }

        var width: Int32 = 0
        var height: Int32 = 0
        var changed: ObjCBool = true
        let pixels = bridge.tick(dt, width: &width, height: &height, changed: &changed)
        let bridge = bridge
        // The table follows the palette fade and the headroom the last frame was drawn with.
        let gain = presenter.highlightGain
        presenter.palette.withUnsafeMutableBufferPointer { table in
            if let base = table.baseAddress {
                bridge.copyPalette(forHeadroom: gain, into: base)
            }
        }
        presenter.present(
            pixels: pixels,
            width: Int(width),
            height: Int(height),
            changed: changed.boolValue
        ) { texture in
            bridge.expand(into: texture)
        }
        // Starts the next frame's warp now, so the GPU runs it while this frame is shown.
        bridge.prepareNextFrame()
        countFrame(at: time)
    }

    private func applySize(_ size: SimulationSize, at time: CFTimeInterval) {
        guard size != simSize else {
            pendingSize = nil
            return
        }
        // Sizes apply at once while the window opens, so it starts on a sharp picture even when
        // its first layout is provisional, such as a restored frame or a launch into fullscreen.
        guard let openedAt else {
            openedAt = time
            apply(size)
            return
        }
        if time - openedAt >= Self.resizeSettleSeconds {
            if size != pendingSize {
                pendingSize = size
                pendingSince = time
                return
            }
            guard time - pendingSince >= Self.resizeSettleSeconds else { return }
        }
        apply(size)
    }

    private func apply(_ size: SimulationSize) {
        simSize = size
        pendingSize = nil
        bridge.resizeWidth(Int32(size.width), height: Int32(size.height))
    }

    // Counts real ticks rather than summing dt, which is capped.
    private func countFrame(at time: CFTimeInterval) {
        fpsWindowFrames += 1
        guard let start = fpsWindowStart else {
            fpsWindowStart = time
            fpsWindowFrames = 0
            return
        }
        let elapsed = time - start
        guard elapsed >= Self.infoInterval else { return }
        fpsWindowStart = time
        defer { fpsWindowFrames = 0 }
        // The timeline stops while the window is hidden. A window spanning that gap
        // would read near zero, so start a fresh one instead.
        guard elapsed <= 2 * Self.infoInterval else { return }
        fps = Int((Double(fpsWindowFrames) / elapsed).rounded())
        if showInfo {
            refreshInfo()
        }
    }

    private func refreshInfo() {
        guard showInfo else {
            // @Published fires on every assignment, so skip a nil that changes nothing.
            if info != nil {
                info = nil
            }
            return
        }
        let look = bridge.lookInfo()
        var wave = "\(look.waveform) \(look.waveformName)"
        if look.waveformHidden {
            wave += " (hidden by Nuclide)"
        }
        let snapshot = InfoSnapshot(
            fps: fps,
            map: "\(look.mapMode) \(look.mapName)",
            wave: wave,
            effects: Self.effectsLine(look),
            palette: look.paletteBlend < 1
                ? String(format: "%d (fading in, %.0f%%)", look.palette, look.paletteBlend * 100)
                : "\(look.palette)",
            hdr: Self.hdrLine(
                enabled: presenter.highlightsOn,
                gain: presenter.highlightGain,
                headroom: presenter.headroom,
                potential: presenter.potentialHeadroom
            ),
            simulation: simulationLine
        )
        if snapshot != info {
            info = snapshot
        }
    }

    // An HDR screen can have no headroom for a moment, for example at full brightness. An SDR
    // screen can offer a little headroom below full brightness, which highlights do not use.
    private static func hdrLine(enabled: Bool, gain: Float, headroom: Float, potential: Float) -> String {
        guard enabled else {
            guard potential > 1 else { return "off (SDR screen)" }
            return String(
                format: "off (screen reaches %.1f×, needs %.1f×)",
                potential,
                MetalPresenter.highlightCap
            )
        }
        guard headroom > 1 else { return String(format: "no headroom now (up to %.1f×)", potential) }
        return String(format: "highlights %.1f× (headroom %.1f× of %.1f×)", gain, headroom, potential)
    }

    private static func effectsLine(_ look: GeissLookInfo) -> String {
        let names = look.effects.isEmpty ? "none" : look.effects.joined(separator: ", ")
        return look.resting ? "\(names) (resting)" : names
    }

    private var simulationLine: String {
        let size = simSize.map { "\($0.width)×\($0.height)" } ?? "–"
        return "\(size) · \(bridge.usesGPU ? "GPU" : "CPU") warp"
    }

    /// The GPU path simulates one pixel per point of the view, as Geiss HDR does per CSS pixel,
    /// up to the GPU's largest texture. The CPU path caps the longest side for its frame rate.
    private func simulationSize(for viewSize: CGSize) -> SimulationSize {
        let cap = bridge.usesGPU ? CGFloat(Self.largestTexture) : Self.cpuLongestSide
        return Self.simulationSize(for: viewSize, longestSide: cap)
    }

    /// Apple Silicon GPUs take 2D textures up to 16384 on a side.
    private static let largestTexture = 16384

    static func simulationSize(for viewSize: CGSize, longestSide: CGFloat) -> SimulationSize {
        let longest = max(viewSize.width, viewSize.height, 1)
        let scale = min(1, longestSide / longest)
        return SimulationSize(
            width: max(2, Int((viewSize.width * scale).rounded())),
            height: max(2, Int((viewSize.height * scale).rounded()))
        )
    }

    private func ingestSystem(dt: Double, time: Double) {
        let running = capture.isCapturing
        if running, let snap = tap.latest(into: &captureBuffer), snap.sequence != lastSequence {
            lastSequence = snap.sequence
            lastCaptureTime = time
            captureBuffer.withUnsafeBufferPointer { buffer in
                bridge.ingestInterleaved(
                    buffer.baseAddress,
                    frameCount: Int32(snap.frames),
                    channels: 2,
                    sampleRate: snap.sampleRate,
                    time: time
                )
            }
            return
        }
        let age = lastCaptureTime.map { time - $0 } ?? 1
        if !running || age > 0.2 {
            ingestSilence(dt: dt, time: time)
        }
    }

    private func ingestSilence(dt: Double, time: Double) {
        let frames = min(max(1, Int(PreviewTone.sampleRate * dt)), Self.maxSilenceFrames)
        silenceBuffer.withUnsafeBufferPointer { buffer in
            bridge.ingestInterleaved(
                buffer.baseAddress,
                frameCount: Int32(frames),
                channels: 2,
                sampleRate: PreviewTone.sampleRate,
                time: time
            )
        }
    }

    private func cappedDelta(to time: CFTimeInterval) -> Double {
        defer { lastTime = time }
        guard let lastTime else { return 1.0 / 60.0 }
        let gap = time - lastTime
        if gap <= 0 || gap > 0.5 {
            return 1.0 / 60.0
        }
        return min(gap, Self.maxDelta)
    }
}

struct SimulationSize: Equatable {
    var width: Int
    var height: Int
}

enum PreviewTone {
    static let sampleRate = 48_000.0
    // As many frames as the capture ring holds, so the tone reaches the analyzer the way
    // system audio does, whatever the frame rate.
    static let frames = 2048

    static func samples(endingAt time: Double) -> [Float] {
        var samples = [Float](repeating: 0, count: frames * 2)
        let frequency = 220.0
        let start = time - Double(frames) / sampleRate
        for index in 0..<frames {
            let sampleTime = start + Double(index + 1) / sampleRate
            let value = Float(sin(sampleTime * frequency * 2 * .pi) * 0.65)
            samples[index * 2] = value
            samples[index * 2 + 1] = value
        }
        return samples
    }
}
