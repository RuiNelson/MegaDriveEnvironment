/**
 * @file VDPPresenterMetal.mm
 * @brief VDP presentation through Metal on macOS: a CAMetalLayer on the SDL window, no SDL_Renderer.
 *
 * Matches the SDL_Renderer path it replaces: the low-power GPU unless SDL's hint says otherwise, a
 * BGRA8 layer whose drawable size SDL's Metal view keeps in step with the window, display sync on,
 * and up to three frames in flight.
 */

#include "VDPPresenter.hpp"
#include "VDP.hpp"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
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

    void present(int width, int height, const m_byte background[3], bool integerScaling, bool linear) override {
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

            MTLRenderPassDescriptor *pass         = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture     = drawable.texture;
            pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor  = MTLClearColorMake(
                background[0] / 255.0, background[1] / 255.0, background[2] / 255.0, 1.0);

            const SDL_FRect dst = VDP::presentationRect(outputW, outputH, width, height, integerScaling);
            const FrameUniforms frame{{static_cast<float>(width), static_cast<float>(height)},
                                      {static_cast<float>(VDPState::MAX_SCREEN_W),
                                       static_cast<float>(VDPState::MAX_SCREEN_H)}};

            id<MTLCommandBuffer>        commands = [queue_ commandBuffer];
            id<MTLRenderCommandEncoder> encoder  = [commands renderCommandEncoderWithDescriptor:pass];
            [encoder setViewport:MTLViewport{dst.x, dst.y, dst.w, dst.h, 0.0, 1.0}];
            [encoder setRenderPipelineState:pipeline_];
            [encoder setVertexBytes:&frame length:sizeof frame atIndex:0];
            [encoder setFragmentBytes:&frame length:sizeof frame atIndex:0];
            [encoder setFragmentTexture:picture atIndex:0];
            [encoder setFragmentSamplerState:(linear ? linear_ : nearest_) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [encoder endEncoding];

            dispatch_semaphore_t inFlight = inFlight_;
            [commands addCompletedHandler:^(id<MTLCommandBuffer>) {
              dispatch_semaphore_signal(inFlight);
            }];
            [commands presentDrawable:drawable];
            [commands commit];
        }
    }

    private:
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
