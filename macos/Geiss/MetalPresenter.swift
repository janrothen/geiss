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

import Metal
import MetalKit
import SwiftUI

final class MetalPresenter: NSObject, MTKViewDelegate {
    let device: MTLDevice
    /// Shared with the GPU simulation, so an expansion runs before the draw that reads it.
    let queue: MTLCommandQueue
    /// Shaders.metal, also holding the simulation's kernels.
    let library: MTLLibrary
    private let pipeline: MTLRenderPipelineState
    /// Uploads rotate through these so the CPU never writes a texture the GPU is still reading.
    private static let textureCount = 3
    /// Written by an upload or by the GPU expansion, as plain 8-bit values.
    private var textures: [MTLTexture] = []
    /// The same textures seen as sRGB, so the texture unit decodes the colors before it filters
    /// them and the shader does not. Alpha, the highlight level, stays linear.
    private var views: [MTLTexture] = []
    private var textureIndex = 0
    private var texture: MTLTexture?
    /// True when the last changed frame never reached a texture, so an unchanged frame after it
    /// fills one again instead of redrawing an older picture.
    private var fillPending = false
    private let inFlight = DispatchSemaphore(value: textureCount)
    /// True between an upload's wait and the draw that releases that slot.
    private var slotHeld = false
    private weak var view: MTKView?
    /// HDR highlights: the hottest pixels rise above SDR white, up to this many
    /// times white or the screen's current headroom, whichever is lower.
    static let highlightCap: Float = 2.5
    /// Whether the screen's view is drawn with highlights. True only on an HDR screen.
    private(set) var highlightsOn = false
    /// The screen's current extended dynamic range headroom. It can fall to 1 on an HDR
    /// screen, for example at full brightness.
    private(set) var headroom: Float = 1
    /// The most headroom the screen can offer. Above 1 only on an HDR screen.
    private(set) var potentialHeadroom: Float = 1
    /// The screen color space the drawable was last tagged for, once a screen is known.
    private var taggedScreenSpace: CGColorSpace?
    /// What the brightest level is drawn at, in multiples of SDR white.
    var highlightGain: Float { highlightsOn ? min(Self.highlightCap, max(1, headroom)) : 1 }

    /// A screen counts as HDR when it can reach the full highlight gain. Many SDR Mac screens
    /// also offer some headroom below full brightness by raising the backlight. Asking for EDR
    /// there would dim everything else on the screen and cost power for a faint glow.
    static func isHDR(potentialHeadroom: Float) -> Bool {
        potentialHeadroom >= highlightCap
    }

    override init() {
        guard let device = MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue()
        else {
            fatalError("Metal is required.")
        }
        self.device = device
        self.queue = queue
        // Shaders.metal ships as a resource and compiles at launch, so building the app needs no
        // separate Metal toolchain.
        guard let url = Bundle.main.url(forResource: "Shaders", withExtension: "metal"),
              let source = try? String(contentsOf: url, encoding: .utf8)
        else {
            fatalError("The Metal shaders are missing.")
        }
        // Without fast math, so the warp kernel rounds like the CPU warp it ports.
        let options = MTLCompileOptions()
        if #available(macOS 15, *) {
            options.mathMode = .safe
            options.mathFloatingPointFunctions = .precise
        } else {
            options.fastMathEnabled = false
        }
        let library: MTLLibrary
        do {
            library = try device.makeLibrary(source: source, options: options)
        } catch {
            fatalError("The Metal shaders do not compile: \(error)")
        }
        self.library = library
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "geissVertex")
        descriptor.fragmentFunction = library.makeFunction(name: "geissFragment")
        descriptor.colorAttachments[0].pixelFormat = .rgba16Float
        pipeline = try! device.makeRenderPipelineState(descriptor: descriptor)
        super.init()
    }

    func attach(_ view: MTKView) {
        self.view = view
        view.device = device
        view.delegate = self
        // A float drawable in an extended linear color space can go brighter than SDR white.
        view.colorPixelFormat = .rgba16Float
        matchScreen(view)
        view.framebufferOnly = true
        view.isPaused = true
        view.enableSetNeedsDisplay = false
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        if texture != nil {
            redraw()
        }
    }

    /// Shows the engine's frame. An unchanged frame of the same size is drawn again from the
    /// last texture without copying it, which is all a paused picture needs: the redraw keeps
    /// the HDR gain and the screen's color space current.
    /// `pixels` is the RGBA frame on the CPU path. On the GPU path it is nil, and `expand`
    /// writes the frame into the texture it is given instead.
    func present(
        pixels: UnsafePointer<UInt8>?,
        width: Int,
        height: Int,
        changed: Bool,
        expand: (MTLTexture) -> Bool
    ) {
        guard width > 0, height > 0 else { return }
        if !changed, !fillPending, let texture, texture.width == width, texture.height == height {
            redraw()
            return
        }
        upload(width: width, height: height) { target in
            if let pixels {
                target.replace(
                    region: MTLRegionMake2D(0, 0, width, height),
                    mipmapLevel: 0,
                    withBytes: pixels,
                    bytesPerRow: width * 4
                )
                return true
            }
            return expand(target)
        }
    }

    /// Draws the last texture again. The draw holds a slot like an upload does, so every command
    /// buffer that reads a texture counts against `inFlight`. The queue finishes its buffers in
    /// order, so once an upload gets a slot, every draw from before the previous two uploads has
    /// finished, and the texture the rotation reuses is no longer being read.
    private func redraw() {
        guard let view else { return }
        inFlight.wait()
        slotHeld = true
        view.draw()
        releaseSlot()
    }

    private func upload(width: Int, height: Int, fill: (MTLTexture) -> Bool) {
        inFlight.wait()
        slotHeld = true
        if textures.first?.width != width || textures.first?.height != height {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .rgba8Unorm,
                width: width,
                height: height,
                mipmapped: false
            )
            descriptor.usage = [.shaderRead, .shaderWrite, .pixelFormatView]
            textures = (0..<Self.textureCount).compactMap { _ in device.makeTexture(descriptor: descriptor) }
            views = textures.compactMap { $0.makeTextureView(pixelFormat: .rgba8Unorm_srgb) }
            textureIndex = 0
        }
        guard !textures.isEmpty, views.count == textures.count else {
            fillPending = true
            releaseSlot()
            return
        }
        let index = textureIndex
        textureIndex = (textureIndex + 1) % textures.count
        guard fill(textures[index]) else {
            fillPending = true
            releaseSlot()
            return
        }
        fillPending = false
        texture = views[index]
        if let view {
            view.draw()
        }
        // No view, or a draw that returned before committing, leaves the slot free.
        releaseSlot()
    }

    private func releaseSlot() {
        if slotHeld {
            slotHeld = false
            inFlight.signal()
        }
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        guard let texture,
              let drawable = view.currentDrawable,
              let pass = view.currentRenderPassDescriptor,
              let buffer = queue.makeCommandBuffer(),
              let encoder = buffer.makeRenderCommandEncoder(descriptor: pass)
        else {
            return
        }
        matchScreen(view)
        var gain = highlightGain
        encoder.setRenderPipelineState(pipeline)
        encoder.setFragmentTexture(texture, index: 0)
        encoder.setFragmentBytes(&gain, length: MemoryLayout<Float>.size, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        encoder.endEncoding()
        if slotHeld {
            // The GPU releases this upload's slot once it has finished reading the texture.
            slotHeld = false
            let semaphore = inFlight
            buffer.addCompletedHandler { _ in
                semaphore.signal()
            }
        }
        buffer.present(drawable)
        buffer.commit()
    }

    /// Keeps EDR, the drawable's color space, and the headroom in step with the view's screen.
    /// Runs every frame: the layer may not exist when the view is attached, the window can move
    /// to another screen, and the headroom follows the brightness setting.
    private func matchScreen(_ view: MTKView) {
        let screen = view.window?.screen
        potentialHeadroom = Float(screen?.maximumPotentialExtendedDynamicRangeColorComponentValue ?? 1)
        highlightsOn = Self.isHDR(potentialHeadroom: potentialHeadroom)
        if let layer = view.layer as? CAMetalLayer, layer.wantsExtendedDynamicRangeContent != highlightsOn {
            layer.wantsExtendedDynamicRangeContent = highlightsOn
        }
        headroom = Float(screen?.maximumExtendedDynamicRangeColorComponentValue ?? 1)
        // The original drew for an unmanaged screen, which showed its values in the
        // screen's own color space. Tagging the drawable with a linear copy of that space keeps
        // those colors on any screen. A screen whose profile has no linear form falls back to
        // Display P3 or sRGB, whichever it can show.
        let screenSpace = screen?.colorSpace?.cgColorSpace
        guard screenSpace != taggedScreenSpace || view.colorspace == nil else { return }
        taggedScreenSpace = screenSpace
        let fallback = (screen?.canRepresent(.p3) ?? false)
            ? CGColorSpace.extendedLinearDisplayP3
            : CGColorSpace.extendedLinearSRGB
        view.colorspace = screenSpace.flatMap { CGColorSpaceCreateExtendedLinearized($0) }
            ?? CGColorSpace(name: fallback)
    }
}

/// An MTKView that calls `onFrame` once per refresh of the screen it is on, up to that
/// screen's highest rate (120 Hz on a ProMotion display), with the time the frame will be
/// shown. Consecutive target times step by exactly the refresh interval, so motion stays even
/// when a callback runs late. The time is on the `CACurrentMediaTime` clock.
final class DisplayLinkedView: MTKView {
    var onFrame: ((CFTimeInterval) -> Void)?
    /// A paused picture does not move, so it asks for a low rate instead of the screen's top rate.
    var lowRate = false {
        didSet {
            if lowRate != oldValue {
                applyRate()
            }
        }
    }
    private var link: CADisplayLink?

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        link?.invalidate()
        link = nil
        guard window != nil else { return }
        // The link follows the view to whichever screen shows it.
        let link = displayLink(target: self, selector: #selector(step))
        link.add(to: .main, forMode: .common)
        self.link = link
        applyRate()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        // A move to a screen with another top rate asks for that rate.
        applyRate()
    }

    private func applyRate() {
        guard let link else { return }
        if lowRate {
            link.preferredFrameRateRange = CAFrameRateRange(minimum: 10, maximum: 20, preferred: 15)
            return
        }
        let fastest = Float(window?.screen?.maximumFramesPerSecond ?? 60)
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 30, maximum: fastest, preferred: fastest)
    }

    @objc private func step(_ link: CADisplayLink) {
        onFrame?(link.targetTimestamp)
    }
}

struct MetalScreen: NSViewRepresentable {
    let presenter: MetalPresenter
    let lowRate: Bool
    let onFrame: (CFTimeInterval) -> Void

    func makeNSView(context: Context) -> DisplayLinkedView {
        let view = DisplayLinkedView()
        presenter.attach(view)
        view.onFrame = onFrame
        view.lowRate = lowRate
        return view
    }

    func updateNSView(_ nsView: DisplayLinkedView, context: Context) {
        nsView.onFrame = onFrame
        nsView.lowRate = lowRate
    }

    static func dismantleNSView(_ nsView: DisplayLinkedView, coordinator: ()) {
        nsView.onFrame = nil
    }
}
