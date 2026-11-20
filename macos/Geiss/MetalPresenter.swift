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
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    /// Uploads rotate through these so the CPU never writes a texture the GPU is still reading.
    private static let textureCount = 3
    private var textures: [MTLTexture] = []
    private var textureIndex = 0
    private var texture: MTLTexture?
    private let inFlight = DispatchSemaphore(value: textureCount)
    /// True between an upload's wait and the draw that releases that slot.
    private var slotHeld = false
    private weak var view: MTKView?
    /// HDR highlights: the brightest palette levels rise above SDR white, up to this many
    /// times white or the screen's current headroom, whichever is lower.
    static let highlightCap: Float = 2.5
    /// The screen's current extended dynamic range headroom. 1 on an SDR screen.
    private(set) var headroom: Float = 1
    /// What the brightest level is drawn at, in multiples of SDR white.
    var highlightGain: Float { min(Self.highlightCap, max(1, headroom)) }

    override init() {
        guard let device = MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue()
        else {
            fatalError("Metal is required.")
        }
        self.device = device
        self.queue = queue
        let library = try! device.makeLibrary(source: Self.shader, options: nil)
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
        // A float drawable in extended linear Display P3 can go brighter than SDR white.
        // Display P3 matches the earlier unmanaged 8-bit output on the Mac's P3 screens.
        view.colorPixelFormat = .rgba16Float
        view.colorspace = CGColorSpace(name: CGColorSpace.extendedLinearDisplayP3)
        (view.layer as? CAMetalLayer)?.wantsExtendedDynamicRangeContent = true
        view.framebufferOnly = true
        view.isPaused = true
        view.enableSetNeedsDisplay = false
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        if texture != nil {
            view.draw()
        }
    }

    func upload(pixels: UnsafePointer<UInt8>, width: Int, height: Int) {
        guard width > 0, height > 0 else { return }
        inFlight.wait()
        slotHeld = true
        if textures.first?.width != width || textures.first?.height != height {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .rgba8Unorm,
                width: width,
                height: height,
                mipmapped: false
            )
            descriptor.usage = [.shaderRead]
            textures = (0..<Self.textureCount).compactMap { _ in device.makeTexture(descriptor: descriptor) }
            textureIndex = 0
        }
        guard !textures.isEmpty else {
            releaseSlot()
            return
        }
        let next = textures[textureIndex]
        textureIndex = (textureIndex + 1) % textures.count
        next.replace(
            region: MTLRegionMake2D(0, 0, width, height),
            mipmapLevel: 0,
            withBytes: pixels,
            bytesPerRow: width * 4
        )
        texture = next
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
        // The headroom follows the screen and its brightness setting, so it is read every frame.
        headroom = Float(view.window?.screen?.maximumExtendedDynamicRangeColorComponentValue ?? 1)
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

    private static let shader = """
    #include <metal_stdlib>
    using namespace metal;

    struct Varying {
        float4 position [[position]];
        float2 uv;
    };

    vertex Varying geissVertex(uint id [[vertex_id]]) {
        float2 xy[4] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };
        float2 uv[4] = { float2(0, 1), float2(1, 1), float2(0, 0), float2(1, 0) };
        Varying out;
        out.position = float4(xy[id], 0, 1);
        out.uv = uv[id];
        return out;
    }

    float3 toLinear(float3 c) {
        return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);
    }

    // RGB is the palette color, encoded like sRGB. Alpha is the palette index. Levels from
    // 80% to the top ease up to `gain` times SDR white, so fresh lines, beat hits, and
    // bright effects glow on an HDR screen while trails keep their SDR brightness.
    fragment float4 geissFragment(Varying in [[stage_in]], texture2d<float> image [[texture(0)]],
                                  constant float& gain [[buffer(0)]]) {
        constexpr sampler linear(filter::linear, address::clamp_to_edge);
        const float4 texel = image.sample(linear, in.uv);
        const float highlight = smoothstep(0.8, 1.0, texel.a);
        return float4(toLinear(texel.rgb) * (1.0 + (gain - 1.0) * highlight), 1.0);
    }
    """
}

/// An MTKView that calls `onFrame` once per refresh of the screen it is on, up to that
/// screen's highest rate (120 Hz on a ProMotion display).
final class DisplayLinkedView: MTKView {
    var onFrame: ((Date) -> Void)?
    private var link: CADisplayLink?

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        link?.invalidate()
        link = nil
        guard window != nil else { return }
        // The link follows the view to whichever screen shows it.
        let link = displayLink(target: self, selector: #selector(step))
        let fastest = Float(window?.screen?.maximumFramesPerSecond ?? 60)
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 30, maximum: fastest, preferred: fastest)
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        // A move to a screen with another top rate asks for that rate.
        if let link, let screen = window?.screen {
            let fastest = Float(screen.maximumFramesPerSecond)
            link.preferredFrameRateRange = CAFrameRateRange(minimum: 30, maximum: fastest, preferred: fastest)
        }
    }

    @objc private func step(_ link: CADisplayLink) {
        onFrame?(.now)
    }
}

struct MetalScreen: NSViewRepresentable {
    let presenter: MetalPresenter
    let onFrame: (Date) -> Void

    func makeNSView(context: Context) -> DisplayLinkedView {
        let view = DisplayLinkedView()
        presenter.attach(view)
        view.onFrame = onFrame
        return view
    }

    func updateNSView(_ nsView: DisplayLinkedView, context: Context) {
        nsView.onFrame = onFrame
    }

    static func dismantleNSView(_ nsView: DisplayLinkedView, coordinator: ()) {
        nsView.onFrame = nil
    }
}
