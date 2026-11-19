/**
 * @file VDPPresenterMetal.mm
 * @brief VDP presentation through Metal on macOS: a CAMetalLayer on the SDL window, no SDL_Renderer.
 *
 * Matches the SDL_Renderer path it replaced: the low-power GPU unless SDL's hint says otherwise, a
 * BGRA8 layer whose drawable size SDL's Metal view keeps in step with the window (in the display's
 * real pixels: the window has SDL_WINDOW_HIGH_PIXEL_DENSITY), display sync on, and up to three frames
 * in flight. Each VDP::ScalingFilter is drawn here: Hybrid in two passes, MetalFX with its spatial
 * scaler.
 */

#include "VDPPresenter.hpp"
#include "VDP.hpp"

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr const char *kShaderSource = R"metal(
#include <metal_stdlib>
using namespace metal;

struct Frame {
    float2 size;        // active picture, in texels
    float2 textureSize; // whole texture, in texels
};

struct VertexOut {
    float4 position [[position]];
    float2 texel;
};

// A quad over the viewport (the presentation rectangle), as a 4-vertex triangle strip.
vertex VertexOut vdpVertex(uint id [[vertex_id]], constant Frame &frame [[buffer(0)]]) {
    const float2 corner = float2(float(id & 1u), float(id >> 1u));
    VertexOut out;
    out.position = float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
    out.texel    = corner * frame.size;
    return out;
}

// Clamped to the active picture's texel centres, so bilinear sampling never reads past its edge.
fragment float4 vdpFragment(VertexOut in [[stage_in]],
                            texture2d<float> picture [[texture(0)]],
                            sampler picker [[sampler(0)]],
                            constant Frame &frame [[buffer(0)]]) {
    const float2 texel = clamp(in.texel, float2(0.5), frame.size - 0.5);
    return float4(picture.sample(picker, texel / frame.textureSize).rgb, 1.0);
}
)metal";

constexpr int kFramesInFlight = 3;
constexpr int kMaxTextureSize = 16384; // every Mac GPU Metal supports

struct FrameUniforms {
    float size[2];
    float textureSize[2];
};

class MetalPresenter final : public VDPPresenter {
    public:
    ~MetalPresenter() override {
        // Let the GPU finish with the textures before they go.
        if (inFlight_) {
            for (int i = 0; i < kFramesInFlight; ++i)
                dispatch_semaphore_wait(inFlight_, DISPATCH_TIME_FOREVER);
            for (int i = 0; i < kFramesInFlight; ++i)
                dispatch_semaphore_signal(inFlight_);
        }
        @autoreleasepool {
            pipeline_ = nil;
            hybrid_   = nil;
            upscaled_ = nil;
            scaler_   = nil;
            nearest_  = nil;
            linear_   = nil;
            for (auto &texture : textures_)
                texture = nil;
            queue_  = nil;
            layer_  = nil;
            device_ = nil;
        }
        if (view_)
            SDL_Metal_DestroyView(view_);
    }

    const char *name() const override {
        return "metal";
    }

    bool init(SDL_Window *window) {
        @autoreleasepool {
            device_ = pickDevice();
            if (device_ == nil)
                return false;
            view_ = SDL_Metal_CreateView(window);
            if (!view_)
                return false;
            layer_ = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(view_);
            if (layer_ == nil)
                return false;
            layer_.device             = device_;
            layer_.pixelFormat        = MTLPixelFormatBGRA8Unorm;
            layer_.framebufferOnly    = YES;
            // Every mode presents in sync with the display, so nothing tears; InternalTimer queues its presents
            // without waiting (VDP::queueAsyncPresent), so this does not pace it.
            layer_.displaySyncEnabled = YES;

            queue_ = [device_ newCommandQueue];
            if (queue_ == nil)
                return false;

            NSError        *error   = nil;
            id<MTLLibrary>  library = [device_ newLibraryWithSource:@(kShaderSource) options:nil error:&error];
            if (library == nil) {
                SDL_Log("[VDP] Metal shaders failed to compile: %s",
                        error ? error.localizedDescription.UTF8String : "unknown error");
                return false;
            }
            MTLRenderPipelineDescriptor *pipeline = [[MTLRenderPipelineDescriptor alloc] init];
            pipeline.vertexFunction                  = [library newFunctionWithName:@"vdpVertex"];
            pipeline.fragmentFunction                = [library newFunctionWithName:@"vdpFragment"];
            pipeline.colorAttachments[0].pixelFormat = layer_.pixelFormat;
            pipeline_ = [device_ newRenderPipelineStateWithDescriptor:pipeline error:&error];
            if (pipeline_ == nil) {
                SDL_Log("[VDP] Metal pipeline failed: %s",
                        error ? error.localizedDescription.UTF8String : "unknown error");
                return false;
            }

            nearest_ = makeSampler(MTLSamplerMinMagFilterNearest);
            linear_  = makeSampler(MTLSamplerMinMagFilterLinear);
            if (nearest_ == nil || linear_ == nil)
                return false;

            MTLTextureDescriptor *picture =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                   width:VDPState::MAX_SCREEN_W
                                                                  height:VDPState::MAX_SCREEN_H
                                                               mipmapped:NO];
            picture.usage = MTLTextureUsageShaderRead;
            if (@available(macOS 13.0, *))
                metalFXSupported_ = [MTLFXSpatialScalerDescriptor supportsDevice:device_];
            for (auto &texture : textures_) {
                texture = [device_ newTextureWithDescriptor:picture];
                if (texture == nil)
                    return false;
            }
        }
        inFlight_ = dispatch_semaphore_create(kFramesInFlight);
        staging_.assign(static_cast<size_t>(VDPState::MAX_SCREEN_W) * VDPState::MAX_SCREEN_H, 0xFF000000u);
        return true;
    }

    void upload(const Framebuffer &framebuffer, int width, int height) override {
        width  = std::clamp(width, 0, Framebuffer::WIDTH);
        height = std::clamp(height, 0, Framebuffer::HEIGHT);
        // 3-bit channels to 8 bits, as Framebuffer::uploadToTexture does; BGRA in memory.
        static constexpr std::uint32_t lut[8] = {0, 36, 73, 109, 146, 182, 219, 255};
        const auto *pixels = static_cast<const m_byte *>(framebuffer.getRawPointer());
        for (int y = 0; y < height; ++y) {
            const m_byte  *source = pixels + y * Framebuffer::PITCH;
            std::uint32_t *target = staging_.data() + static_cast<size_t>(y) * VDPState::MAX_SCREEN_W;
            for (int x = 0; x < width; ++x, source += Framebuffer::BPP) {
                target[x] = 0xFF000000u | (lut[source[2] & 0x07] << 16) | (lut[source[1] & 0x07] << 8) |
                            lut[source[0] & 0x07];
            }
        }
        uploadedWidth_  = width;
        uploadedHeight_ = height;
    }

    bool supports(VDP::ScalingFilter filter) const override {
        return filter != VDP::MetalFX || metalFXSupported_;
    }

    void present(int width, int height, const m_byte background[3], bool integerScaling, VDP::ScalingFilter filter) override {
        width  = std::min(width, uploadedWidth_);
        height = std::min(height, uploadedHeight_);
        if (width <= 0 || height <= 0)
            return;
        @autoreleasepool {
            const CGSize drawableSize = layer_.drawableSize;
            const int    outputW      = static_cast<int>(drawableSize.width);
            const int    outputH      = static_cast<int>(drawableSize.height);
            if (outputW <= 0 || outputH <= 0)
                return;

            dispatch_semaphore_wait(inFlight_, DISPATCH_TIME_FOREVER);
            id<CAMetalDrawable> drawable = [layer_ nextDrawable];
            if (drawable == nil) {
                dispatch_semaphore_signal(inFlight_);
                return;
            }

            id<MTLTexture> picture = textures_[slot_];
            slot_                  = (slot_ + 1) % kFramesInFlight;
            [picture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                       mipmapLevel:0
                         withBytes:staging_.data()
                       bytesPerRow:VDPState::MAX_SCREEN_W * sizeof(std::uint32_t)];

            const SDL_FRect dst     = VDP::presentationRect(outputW, outputH, width, height, integerScaling);
            const int       targetW = std::max(1, static_cast<int>(std::lround(dst.w)));
            const int       targetH = std::max(1, static_cast<int>(std::lround(dst.h)));

            id<MTLCommandBuffer> commands = [queue_ commandBuffer];
            // What the last pass draws into the rectangle: the picture itself, or a pass's output.
            id<MTLTexture> source  = picture;
            float          sourceW = static_cast<float>(width);
            float          sourceH = static_cast<float>(height);
            bool           linear  = filter == VDP::Bilinear;
            MTLViewport    place{dst.x, dst.y, dst.w, dst.h, 0.0, 1.0};

            if (filter == VDP::Hybrid) {
                // Nearest-neighbour to the next whole multiple, then bilinear down to the rectangle; a whole
                // multiple already is nearest-neighbour alone.
                const int factor = VDP::hybridFactor(dst.w, width);
                if (static_cast<float>(factor * width) - dst.w > 0.5f) {
                    const int      scaledW = std::min(factor * width, kMaxTextureSize);
                    const int      scaledH = std::min(factor * height, kMaxTextureSize);
                    id<MTLTexture> scaled  = scratch(hybrid_, scaledW, scaledH, MTLTextureUsageRenderTarget);
                    if (scaled != nil) {
                        draw(commands, scaled, nullptr, MTLViewport{0, 0, double(scaledW), double(scaledH), 0, 1},
                             picture, sourceW, sourceH, false);
                        source  = scaled;
                        sourceW = static_cast<float>(scaledW);
                        sourceH = static_cast<float>(scaledH);
                        linear  = true;
                    }
                }
            } else if (filter == VDP::MetalFX) {
                id<MTLTexture> upscaled = nil;
                if (@available(macOS 13.0, *))
                    upscaled = metalFX(commands, picture, width, height, targetW, targetH);
                if (upscaled != nil) {
                    // Already the rectangle's size: copied pixel for pixel.
                    source  = upscaled;
                    sourceW = static_cast<float>(targetW);
                    sourceH = static_cast<float>(targetH);
                    place   = MTLViewport{dst.x, dst.y, double(targetW), double(targetH), 0, 1};
                } else {
                    linear = true;
                }
            }

            const MTLClearColor clear =
                MTLClearColorMake(background[0] / 255.0, background[1] / 255.0, background[2] / 255.0, 1.0);
            draw(commands, drawable.texture, &clear, place, source, sourceW, sourceH, linear);

            dispatch_semaphore_t inFlight = inFlight_;
            [commands addCompletedHandler:^(id<MTLCommandBuffer>) {
              dispatch_semaphore_signal(inFlight);
            }];
            [commands presentDrawable:drawable];
            [commands commit];
        }
    }

    private:
    /// One render pass: @p source's top-left sourceW×sourceH texels into @p viewport of @p target, which is
    /// cleared to @p clear first when given.
    void draw(id<MTLCommandBuffer> commands,
              id<MTLTexture>       target,
              const MTLClearColor *clear,
              MTLViewport          viewport,
              id<MTLTexture>       source,
              float                sourceW,
              float                sourceH,
              bool                 linear) {
        MTLRenderPassDescriptor *pass         = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture     = target;
        pass.colorAttachments[0].loadAction  = clear ? MTLLoadActionClear : MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (clear)
            pass.colorAttachments[0].clearColor = *clear;
        const FrameUniforms frame{{sourceW, sourceH},
                                  {static_cast<float>(source.width), static_cast<float>(source.height)}};
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        [encoder setViewport:viewport];
        [encoder setRenderPipelineState:pipeline_];
        [encoder setVertexBytes:&frame length:sizeof frame atIndex:0];
        [encoder setFragmentBytes:&frame length:sizeof frame atIndex:0];
        [encoder setFragmentTexture:source atIndex:0];
        [encoder setFragmentSamplerState:(linear ? linear_ : nearest_) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [encoder endEncoding];
    }

    /// A GPU-only BGRA8 texture of w×h in @p slot, remade when the size changes. The GPU orders the passes
    /// of successive frames on the one queue, so a single one serves every frame in flight.
    id<MTLTexture> scratch(id<MTLTexture> __strong &slot, int w, int h, MTLTextureUsage usage) {
        if (slot != nil && static_cast<int>(slot.width) == w && static_cast<int>(slot.height) == h)
            return slot;
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                              width:w
                                                                                             height:h
                                                                                          mipmapped:NO];
        descriptor.usage       = usage | MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModePrivate;
        slot                   = [device_ newTextureWithDescriptor:descriptor];
        return slot;
    }

    /// MetalFX's spatial upscale of the picture's width×height to outW×outH; nil when it cannot be made.
    id<MTLTexture> metalFX(id<MTLCommandBuffer> commands, id<MTLTexture> picture, int width, int height, int outW, int outH)
        API_AVAILABLE(macos(13.0)) {
        id<MTLFXSpatialScaler> scaler = scaler_;
        if (scaler == nil || static_cast<int>(scaler.outputWidth) != outW || static_cast<int>(scaler.outputHeight) != outH) {
            MTLFXSpatialScalerDescriptor *descriptor = [[MTLFXSpatialScalerDescriptor alloc] init];
            descriptor.inputWidth          = VDPState::MAX_SCREEN_W; // the picture texture; its content varies
            descriptor.inputHeight         = VDPState::MAX_SCREEN_H;
            descriptor.outputWidth         = outW;
            descriptor.outputHeight        = outH;
            descriptor.colorTextureFormat  = MTLPixelFormatBGRA8Unorm;
            descriptor.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
            descriptor.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
            scaler                         = [descriptor newSpatialScalerWithDevice:device_];
            if (scaler == nil || (scaler.colorTextureUsage & ~picture.usage) != 0) {
                SDL_Log("[VDP] MetalFX cannot scale %dx%d to %dx%d", width, height, outW, outH);
                scaler_ = nil;
                return nil;
            }
            scaler_ = scaler;
        }
        id<MTLTexture> output = scratch(upscaled_, outW, outH, scaler.outputTextureUsage);
        if (output == nil)
            return nil;
        scaler.colorTexture       = picture;
        scaler.outputTexture      = output;
        scaler.inputContentWidth  = width;
        scaler.inputContentHeight = height;
        [scaler encodeToCommandBuffer:commands];
        return output;
    }

    static id<MTLDevice> pickDevice() {
        // As SDL's Metal renderer: the integrated GPU of a dual-GPU Mac, so the game does not wake the other one.
        if (SDL_GetHintBoolean(SDL_HINT_RENDER_METAL_PREFER_LOW_POWER_DEVICE, true)) {
            for (id<MTLDevice> device in MTLCopyAllDevices()) {
                if (device.isLowPower)
                    return device;
            }
        }
        return MTLCreateSystemDefaultDevice();
    }

    id<MTLSamplerState> makeSampler(MTLSamplerMinMagFilter filter) {
        MTLSamplerDescriptor *sampler = [[MTLSamplerDescriptor alloc] init];
        sampler.minFilter             = filter;
        sampler.magFilter             = filter;
        sampler.sAddressMode          = MTLSamplerAddressModeClampToEdge;
        sampler.tAddressMode          = MTLSamplerAddressModeClampToEdge;
        return [device_ newSamplerStateWithDescriptor:sampler];
    }

    SDL_MetalView              view_ = nullptr;
    id<MTLDevice>              device_;
    CAMetalLayer              *layer_;
    id<MTLCommandQueue>        queue_;
    id<MTLRenderPipelineState> pipeline_;
    id<MTLSamplerState>        nearest_;
    id<MTLSamplerState>        linear_;
    id<MTLTexture>             textures_[kFramesInFlight];
    id<MTLTexture>             hybrid_;   // Hybrid's whole-multiple pass
    id<MTLTexture>             upscaled_; // MetalFX's output
    id                         scaler_;   // id<MTLFXSpatialScaler>, macOS 13
    bool                       metalFXSupported_ = false;
    int                        slot_ = 0;
    dispatch_semaphore_t       inFlight_;
    std::vector<std::uint32_t> staging_;
    int                        uploadedWidth_  = 0;
    int                        uploadedHeight_ = 0;
};

} // namespace

std::unique_ptr<VDPPresenter> createMetalPresenter(SDL_Window *window) {
    auto presenter = std::make_unique<MetalPresenter>();
    if (!presenter->init(window))
        return nullptr;
    return presenter;
}
