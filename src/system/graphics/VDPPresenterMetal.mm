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
#include <cstdio>
#include <string>
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

// Frame generation without pose blending: where the picture-sized hold texture says so, the previous real frame,
// moved by the hold's offset (picture pixels), instead of the generated frame. frame.size is the output's size
// and frame.textureSize the picture's.
fragment float4 vdpHold(VertexOut in [[stage_in]],
                        texture2d<float> generated [[texture(0)]],
                        texture2d<float> previous [[texture(1)]],
                        texture2d<float> holds [[texture(2)]],
                        sampler picker [[sampler(0)]],
                        constant Frame &frame [[buffer(0)]]) {
    const float2 uv   = in.texel / frame.size;
    const float4 hold = holds.sample(picker, uv);
    if (hold.z < 0.5)
        return float4(generated.sample(picker, uv).rgb, 1.0);
    return float4(previous.sample(picker, clamp(uv + hold.xy / frame.textureSize, 0.0, 1.0)).rgb, 1.0);
}
)metal";

constexpr int kFramesInFlight = 3;
constexpr int kMaxTextureSize = 16384; // every Mac GPU Metal supports
// Frame generation's scaled pictures: drawn into, read by the interpolator and by the last pass.
constexpr MTLTextureUsage kFrameUsage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;

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
            interpolator_ = nil;
            frames_[0] = frames_[1] = nil;
            between_  = nil;
            held_     = nil;
            holdPipeline_ = nil;
            for (auto &texture : holds_)
                texture = nil;
            depth_    = nil;
            for (auto &texture : motion_)
                texture = nil;
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
            pipeline.fragmentFunction = [library newFunctionWithName:@"vdpHold"];
            holdPipeline_             = [device_ newRenderPipelineStateWithDescriptor:pipeline error:&error];
            if (pipeline_ == nil || holdPipeline_ == nil) {
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
            if (@available(macOS 26.0, *))
                frameGenerationSupported_ = [MTLFXFrameInterpolatorDescriptor supportsDevice:device_];
            // Development aids: MDE_FRAMEGEN_DEBUG=stats logs the real and generated frames each second, =generated
            // also shows the generated frames in place of the real ones; MDE_FRAMEGEN_MOTION_SCALE multiplies the
            // motion vectors.
            if (const char *debug = SDL_getenv("MDE_FRAMEGEN_DEBUG")) {
                debugStats_     = true;
                debugGenerated_ = SDL_strcmp(debug, "generated") == 0;
            }
            // MDE_FRAMEGEN_DUMP=PREFIX writes the generated frame MDE_FRAMEGEN_DUMP_AT (120 by default) and the
            // two around it as PREFIX-previous.ppm, PREFIX-between.ppm and PREFIX-current.ppm.
            if (const char *dump = SDL_getenv("MDE_FRAMEGEN_DUMP"))
                dumpPrefix_ = dump;
            if (const char *at = SDL_getenv("MDE_FRAMEGEN_DUMP_AT"))
                dumpAt_ = SDL_atoi(at);
            if (const char *scale = SDL_getenv("MDE_FRAMEGEN_MOTION_SCALE"))
                motionScale_ = static_cast<float>(SDL_atof(scale));
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

    bool supportsFrameGeneration() const override {
        return frameGenerationSupported_;
    }

    void present(int                       width,
                 int                       height,
                 const m_byte              background[3],
                 bool                      integerScaling,
                 VDP::ScalingFilter        filter,
                 const VDPFrameGeneration &generation) override {
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
            id<MTLTexture> picture = textures_[slot_];
            [picture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                       mipmapLevel:0
                         withBytes:staging_.data()
                       bytesPerRow:VDPState::MAX_SCREEN_W * sizeof(std::uint32_t)];

            const SDL_FRect dst     = VDP::presentationRect(outputW, outputH, width, height, integerScaling);
            const int       targetW = std::max(1, static_cast<int>(std::lround(dst.w)));
            const int       targetH = std::max(1, static_cast<int>(std::lround(dst.h)));
            const MTLClearColor clear =
                MTLClearColorMake(background[0] / 255.0, background[1] / 255.0, background[2] / 255.0, 1.0);

            id<MTLCommandBuffer> commands = [queue_ commandBuffer];
            const Scaled         scaled   = scale(commands, picture, width, height, dst, targetW, targetH, filter);
            bool                 presented = false;

            if (!generation.keep) {
                historyValid_ = false;
            } else if (@available(macOS 26.0, *)) {
                // The scaled picture goes into a texture of its own, kept for the next interpolation.
                const int      current = 1 - historySlot_;
                id<MTLTexture> frame   = scratch(frames_[current], targetW, targetH, kFrameUsage);
                if (frame != nil) {
                    draw(commands, frame, nullptr, MTLViewport{0, 0, double(targetW), double(targetH), 0, 1},
                         scaled.source, scaled.sourceW, scaled.sourceH, scaled.linear);
                    id<MTLTexture> previous = frames_[historySlot_];
                    const bool     history  = historyValid_ && previous != nil &&
                                         static_cast<int>(previous.width) == targetW &&
                                         static_cast<int>(previous.height) == targetH && historyWidth_ == width &&
                                         historyHeight_ == height;
                    const MTLViewport place{dst.x, dst.y, double(targetW), double(targetH), 0, 1};
                    // Both frames wait half the interval after the one before, so they share it evenly
                    // whatever the display's rate (a little less, so a late present does not miss a refresh).
                    const double half = generation.interval * 0.5 * 0.9;
                    id<MTLTexture> between =
                        history && generation.motion != nullptr
                            ? interpolate(commands, previous, frame, generation, width, height, targetW, targetH)
                            : nil;
                    if (between != nil && generation.holds != nullptr)
                        between = holdPoses(commands, between, previous, generation.holds, width, height, targetW, targetH);
                    if (between != nil) {
                        id<CAMetalDrawable> first = [layer_ nextDrawable];
                        if (first != nil) {
                            draw(commands, first.texture, &clear, place, between, float(targetW), float(targetH),
                                 false);
                            [commands presentDrawable:first afterMinimumDuration:half];
                        }
                    }
                    id<CAMetalDrawable> drawable = [layer_ nextDrawable];
                    if (drawable != nil) {
                        draw(commands, drawable.texture, &clear, place, debugGenerated_ && between ? between : frame,
                             float(targetW), float(targetH), false);
                        if (between != nil)
                            [commands presentDrawable:drawable afterMinimumDuration:half];
                        else
                            [commands presentDrawable:drawable];
                    }
                    if (between != nil && !dumpPrefix_.empty() && ++dumpCountdown_ == dumpAt_) {
                        dumpTexture(commands, previous, dumpPrefix_ + "-previous.ppm");
                        dumpTexture(commands, between, dumpPrefix_ + "-between.ppm");
                        dumpTexture(commands, frame, dumpPrefix_ + "-current.ppm");
                    }
                    if (debugStats_) {
                        ++statsReal_;
                        statsGenerated_ += between != nil ? 1 : 0;
                        const std::uint64_t now = SDL_GetTicksNS();
                        if (now - statsSince_ >= 1'000'000'000ull) {
                            SDL_Log("[VDP] frame generation: %d real, %d generated", statsReal_, statsGenerated_);
                            statsReal_ = statsGenerated_ = 0;
                            statsSince_                  = now;
                        }
                    }
                    historySlot_   = current;
                    historyValid_  = true;
                    historyWidth_  = width;
                    historyHeight_ = height;
                    presented      = true;
                }
            }
            if (!presented) {
                id<CAMetalDrawable> drawable = [layer_ nextDrawable];
                if (drawable != nil) {
                    draw(commands, drawable.texture, &clear, scaled.place, scaled.source, scaled.sourceW, scaled.sourceH,
                         scaled.linear);
                    [commands presentDrawable:drawable];
                }
            }

            slot_                         = (slot_ + 1) % kFramesInFlight;
            dispatch_semaphore_t inFlight = inFlight_;
            [commands addCompletedHandler:^(id<MTLCommandBuffer>) {
              dispatch_semaphore_signal(inFlight);
            }];
            [commands commit];
        }
    }

    private:
    /// What the last pass draws into the presentation rectangle (see scale()).
    struct Scaled {
        id<MTLTexture> source;
        float          sourceW;
        float          sourceH;
        bool           linear;
        MTLViewport    place;
    };

    /// Encodes the passes @p filter needs before the last one (Hybrid's whole multiple, MetalFX's upscale)
    /// and says what the last one draws, and where.
    Scaled scale(id<MTLCommandBuffer> commands,
                 id<MTLTexture>       picture,
                 int                  width,
                 int                  height,
                 const SDL_FRect     &dst,
                 int                  targetW,
                 int                  targetH,
                 VDP::ScalingFilter   filter) {
        Scaled result{picture, float(width), float(height), filter == VDP::Bilinear,
                      MTLViewport{dst.x, dst.y, dst.w, dst.h, 0.0, 1.0}};
        if (filter == VDP::Hybrid) {
            // Nearest-neighbour to the next whole multiple, then bilinear down to the rectangle; a whole
            // multiple already is nearest-neighbour alone.
            const int factor = VDP::hybridFactor(dst.w, width);
            if (static_cast<float>(factor * width) - dst.w > 0.5f) {
                const int      scaledW = std::min(factor * width, kMaxTextureSize);
                const int      scaledH = std::min(factor * height, kMaxTextureSize);
                id<MTLTexture> whole   = scratch(hybrid_, scaledW, scaledH, MTLTextureUsageRenderTarget);
                if (whole != nil) {
                    draw(commands, whole, nullptr, MTLViewport{0, 0, double(scaledW), double(scaledH), 0, 1}, picture,
                         float(width), float(height), false);
                    result.source  = whole;
                    result.sourceW = float(scaledW);
                    result.sourceH = float(scaledH);
                    result.linear  = true;
                }
            }
        } else if (filter == VDP::MetalFX) {
            id<MTLTexture> upscaled = nil;
            if (@available(macOS 13.0, *))
                upscaled = metalFX(commands, picture, width, height, targetW, targetH);
            if (upscaled != nil) {
                // Already the rectangle's size: copied pixel for pixel.
                result.source  = upscaled;
                result.sourceW = float(targetW);
                result.sourceH = float(targetH);
                result.linear  = false;
                result.place   = MTLViewport{dst.x, dst.y, double(targetW), double(targetH), 0, 1};
            } else {
                result.linear = true;
            }
        }
        return result;
    }

    /// MetalFX's frame between @p previous and @p current (both targetW×targetH), from the motion of the
    /// width×height picture; nil when the interpolator cannot be made.
    id<MTLTexture> interpolate(id<MTLCommandBuffer>      commands,
                               id<MTLTexture>            previous,
                               id<MTLTexture>            current,
                               const VDPFrameGeneration &generation,
                               int                       width,
                               int                       height,
                               int                       targetW,
                               int                       targetH) API_AVAILABLE(macos(26.0)) {
        id<MTLFXFrameInterpolator> interpolator = interpolator_;
        bool                       reset        = false;
        if (interpolator == nil || static_cast<int>(interpolator.inputWidth) != width ||
            static_cast<int>(interpolator.inputHeight) != height ||
            static_cast<int>(interpolator.outputWidth) != targetW ||
            static_cast<int>(interpolator.outputHeight) != targetH) {
            MTLFXFrameInterpolatorDescriptor *descriptor = [[MTLFXFrameInterpolatorDescriptor alloc] init];
            descriptor.colorTextureFormat  = MTLPixelFormatBGRA8Unorm;
            descriptor.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
            descriptor.depthTextureFormat  = MTLPixelFormatDepth32Float;
            descriptor.motionTextureFormat = MTLPixelFormatRG16Float;
            descriptor.inputWidth          = width;
            descriptor.inputHeight         = height;
            descriptor.outputWidth         = targetW;
            descriptor.outputHeight        = targetH;
            interpolator                   = [descriptor newFrameInterpolatorWithDevice:device_];
            interpolator_                  = interpolator;
            if (interpolator == nil || (interpolator.colorTextureUsage & ~current.usage) != 0) {
                SDL_Log("[VDP] MetalFX cannot interpolate %dx%d to %dx%d", width, height, targetW, targetH);
                interpolator_ = nil;
                return nil;
            }
            // A 2D picture has no depth: one plane, cleared once.
            MTLTextureDescriptor *depth =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                   width:width
                                                                  height:height
                                                               mipmapped:NO];
            depth.usage       = interpolator.depthTextureUsage | MTLTextureUsageRenderTarget;
            depth.storageMode = MTLStorageModePrivate;
            depth_            = [device_ newTextureWithDescriptor:depth];
            MTLRenderPassDescriptor *clearDepth   = [MTLRenderPassDescriptor renderPassDescriptor];
            clearDepth.depthAttachment.texture     = depth_;
            clearDepth.depthAttachment.loadAction  = MTLLoadActionClear;
            clearDepth.depthAttachment.storeAction = MTLStoreActionStore;
            clearDepth.depthAttachment.clearDepth  = 0.5;
            [[commands renderCommandEncoderWithDescriptor:clearDepth] endEncoding];

            MTLTextureDescriptor *motion =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float
                                                                   width:width
                                                                  height:height
                                                               mipmapped:NO];
            motion.usage = interpolator.motionTextureUsage | MTLTextureUsageShaderRead;
            for (auto &texture : motion_)
                texture = [device_ newTextureWithDescriptor:motion];
            reset = true;
        }
        id<MTLTexture> output = scratch(between_, targetW, targetH, interpolator.outputTextureUsage);
        if (output == nil || depth_ == nil || motion_[slot_] == nil)
            return nil;

        // Picture pixels, as half floats.
        motionHalf_.resize(static_cast<size_t>(width) * height * 2);
        for (size_t i = 0; i < motionHalf_.size(); ++i)
            motionHalf_[i] = static_cast<_Float16>(generation.motion[i]);
        [motion_[slot_] replaceRegion:MTLRegionMake2D(0, 0, width, height)
                          mipmapLevel:0
                            withBytes:motionHalf_.data()
                          bytesPerRow:static_cast<NSUInteger>(width) * 2 * sizeof(_Float16)];

        interpolator.colorTexture       = current;
        interpolator.prevColorTexture   = previous;
        interpolator.depthTexture       = depth_;
        interpolator.motionTexture      = motion_[slot_];
        interpolator.outputTexture      = output;
        interpolator.motionVectorScaleX = motionScale_;
        interpolator.motionVectorScaleY = motionScale_;
        interpolator.deltaTime          = static_cast<float>(generation.interval);
        interpolator.nearPlane          = 0.1f;
        interpolator.farPlane           = 100.0f;
        interpolator.fieldOfView        = 60.0f;
        interpolator.aspectRatio        = static_cast<float>(targetW) / static_cast<float>(targetH);
        interpolator.jitterOffsetX      = 0.0f;
        interpolator.jitterOffsetY      = 0.0f;
        interpolator.shouldResetHistory = reset;
        [interpolator encodeToCommandBuffer:commands];
        return output;
    }

    /// The generated frame with the pose holds (VDPFrameGeneration::holds) taken from the previous frame.
    id<MTLTexture> holdPoses(id<MTLCommandBuffer> commands,
                             id<MTLTexture>       generated,
                             id<MTLTexture>       previous,
                             const float         *holds,
                             int                  width,
                             int                  height,
                             int                  targetW,
                             int                  targetH) {
        id<MTLTexture> __strong &holdTexture = holds_[slot_];
        if (holdTexture == nil || static_cast<int>(holdTexture.width) != width ||
            static_cast<int>(holdTexture.height) != height) {
            MTLTextureDescriptor *descriptor =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                   width:width
                                                                  height:height
                                                               mipmapped:NO];
            descriptor.usage = MTLTextureUsageShaderRead;
            holdTexture      = [device_ newTextureWithDescriptor:descriptor];
        }
        id<MTLTexture> output = scratch(held_, targetW, targetH, MTLTextureUsageRenderTarget);
        if (holdTexture == nil || output == nil)
            return generated;
        holdsHalf_.resize(static_cast<size_t>(width) * height * 4);
        for (size_t i = 0; i < holdsHalf_.size(); ++i)
            holdsHalf_[i] = static_cast<_Float16>(holds[i]);
        [holdTexture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                       mipmapLevel:0
                         withBytes:holdsHalf_.data()
                       bytesPerRow:static_cast<NSUInteger>(width) * 4 * sizeof(_Float16)];

        MTLRenderPassDescriptor *pass         = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture     = output;
        pass.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        const FrameUniforms frame{{float(targetW), float(targetH)}, {float(width), float(height)}};
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        [encoder setViewport:MTLViewport{0, 0, double(targetW), double(targetH), 0, 1}];
        [encoder setRenderPipelineState:holdPipeline_];
        [encoder setVertexBytes:&frame length:sizeof frame atIndex:0];
        [encoder setFragmentBytes:&frame length:sizeof frame atIndex:0];
        [encoder setFragmentTexture:generated atIndex:0];
        [encoder setFragmentTexture:previous atIndex:1];
        [encoder setFragmentTexture:holdTexture atIndex:2];
        [encoder setFragmentSamplerState:nearest_ atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [encoder endEncoding];
        return output;
    }

    /// Development aid: copies @p texture (BGRA8) to a PPM file once the GPU is done.
    void dumpTexture(id<MTLCommandBuffer> commands, id<MTLTexture> texture, std::string path) {
        const NSUInteger w = texture.width, h = texture.height, pitch = w * 4;
        id<MTLBuffer>    buffer = [device_ newBufferWithLength:pitch * h options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        [blit copyFromTexture:texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, h, 1)
                            toBuffer:buffer
                   destinationOffset:0
              destinationBytesPerRow:pitch
            destinationBytesPerImage:pitch * h];
        [blit endEncoding];
        [commands addCompletedHandler:^(id<MTLCommandBuffer>) {
          if (FILE *file = std::fopen(path.c_str(), "wb")) {
              std::fprintf(file, "P6\n%lu %lu\n255\n", static_cast<unsigned long>(w), static_cast<unsigned long>(h));
              const auto *bgra = static_cast<const std::uint8_t *>(buffer.contents);
              for (NSUInteger i = 0; i < w * h; ++i) {
                  const std::uint8_t rgb[3] = {bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4]};
                  std::fwrite(rgb, 1, 3, file);
              }
              std::fclose(file);
          }
        }];
    }

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
    id<MTLRenderPipelineState> holdPipeline_;
    id<MTLSamplerState>        nearest_;
    id<MTLSamplerState>        linear_;
    id<MTLTexture>             textures_[kFramesInFlight];
    id<MTLTexture>             hybrid_;   // Hybrid's whole-multiple pass
    id<MTLTexture>             upscaled_; // MetalFX's output
    id                         scaler_;   // id<MTLFXSpatialScaler>, macOS 13
    bool                       metalFXSupported_ = false;
    // Frame generation (macOS 26).
    id                         interpolator_; // id<MTLFXFrameInterpolator>
    id<MTLTexture>             frames_[2];    // the scaled pictures, current and kept
    int                        historySlot_   = 0;
    bool                       historyValid_  = false;
    int                        historyWidth_  = 0;
    int                        historyHeight_ = 0;
    id<MTLTexture>             between_;      // the generated frame
    id<MTLTexture>             depth_;
    id<MTLTexture>             motion_[kFramesInFlight];
    std::vector<_Float16>      motionHalf_;
    id<MTLTexture>             holds_[kFramesInFlight]; // pose holds, picture-sized
    id<MTLTexture>             held_;                    // the generated frame with them
    std::vector<_Float16>      holdsHalf_;
    bool                       frameGenerationSupported_ = false;
    bool                       debugGenerated_           = false;
    bool                       debugStats_               = false;
    std::string                dumpPrefix_;
    int                        dumpCountdown_            = 0;
    int                        dumpAt_                   = 120;
    int                        statsReal_                = 0;
    int                        statsGenerated_           = 0;
    std::uint64_t              statsSince_               = 0;
    float                      motionScale_              = 1.0f;
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
