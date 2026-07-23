/**
 * @file VDP.cpp
 * @brief Sega Mega Drive VDP emulator — composite implementation.
 */

#include "VDP.hpp"
#include "system/MegaDriveEnvironment.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <png.h>

// ── Construction / Destruction ──────────────────────────────────────────────

/// Initializes SDL, creates window and renderer, and resets VDP state. The
/// render thread is not spawned here — call start() once the environment is
/// fully constructed.
VDP::VDP(MegaDriveEnvironment *env, Synchronization synchronization, Scaling scaling, SpriteLimitMode spriteLimitMode)
    : state_(), tile_(state_), port_(state_), renderer_(state_, tile_, framebuffer_),
      rendererDebug_(state_, tile_, framebuffer_), syncMode_(synchronization), scalingMode_(scaling), env_(env) {
    SDL_Init(SDL_INIT_VIDEO);
    mutex_    = SDL_CreateMutex();
    irqMutex_ = SDL_CreateMutex();
    port_.setEnvironment(env);
    renderer_.setHardwareSpriteLimits(spriteLimitMode == HardwareSpriteLimit);
    state_.reset();

    int winW = VDPState::SCREEN_W;
    int winH = VDPState::SCREEN_H;
    if (scalingMode_ > 0) {
        winW *= scalingMode_;
        winH *= scalingMode_;
    } else if (scalingMode_ == Integer) {
        SDL_Rect      usable{};
        SDL_DisplayID display = SDL_GetPrimaryDisplay();
        if (display && SDL_GetDisplayUsableBounds(display, &usable)) {
            int scaleX = usable.w / VDPState::SCREEN_W;
            int scaleY = usable.h / VDPState::SCREEN_H;
            int scale  = std::max(1, std::min(scaleX, scaleY));
            winW *= scale;
            winH *= scale;
            SDL_Log("[VDP] Display usable: %dx%d  scaleX=%d scaleY=%d  chosen scale=%d  window=%dx%d",
                    usable.w,
                    usable.h,
                    scaleX,
                    scaleY,
                    scale,
                    winW,
                    winH);
        } else {
            winW *= 3;
            winH *= 3;
            SDL_Log("[VDP] SDL_GetDisplayUsableBounds failed, falling back to 3x: window=%dx%d", winW, winH);
        }
    } else {
        winW *= 3;
        winH *= 3;
    }

    window_ = SDL_CreateWindow("VDP", winW, winH, SDL_WINDOW_RESIZABLE);
    if (window_) {
        sdlRenderer_ = SDL_CreateRenderer(window_, nullptr);
        if (sdlRenderer_) {
            if (syncMode_ != InternalTimer) {
                SDL_SetRenderVSync(sdlRenderer_, 1);
            }
            texture_ = SDL_CreateTexture(sdlRenderer_,
                                         SDL_PIXELFORMAT_BGR24,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         VDPState::MAX_SCREEN_W,
                                         VDPState::MAX_SCREEN_H);
            if (texture_) {
                SDL_SetTextureScaleMode(texture_, (scalingMode_ == Fit) ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
            }
        }
    }
}

VDP::~VDP() {
    shutdown();
}

// ── Threading ──────────────────────────────────────────────────────────────

/// Spawns the render thread. No-op if it is already running.
void VDP::start() {
    if (thread_) {
        return;
    }
    debugFrame_ = 0;
    nextDebugLogNs_ = SDL_GetTicksNS() + 1'000'000'000ull;
    running_ = true;
    thread_  = SDL_CreateThread(renderThreadEntry, "VDP", this);
}

/// Stops the render thread, keeping the SDL window/renderer alive for a later start().
void VDP::stop() {
    running_ = false;
    wakeSyncWaiters();
    if (thread_) {
        SDL_Event e;
        while (SDL_GetThreadState(thread_) != SDL_THREAD_COMPLETE) {
            SDL_PollEvent(&e);
            SDL_Delay(1);
        }
        SDL_WaitThread(thread_, nullptr);
        thread_ = nullptr;
    }
}

VDP::FramebufferSnapshot VDP::framebufferSnapshot() const {
    FramebufferSnapshot result;
    SDL_LockMutex(mutex_);
    result.width = static_cast<std::uint16_t>(state_.activeWidth());
    result.height = static_cast<std::uint16_t>(state_.activeOutputHeight());
    result.pitch = static_cast<std::uint16_t>(result.width * Framebuffer::BPP);
    result.pixels.resize(static_cast<std::size_t>(result.pitch) * result.height);
    const auto *source = static_cast<const m_byte *>(framebuffer_.getRawPointer());
    for (std::uint16_t y = 0; y < result.height; ++y) {
        std::memcpy(result.pixels.data() + static_cast<std::size_t>(y) * result.pitch,
                    source + static_cast<std::size_t>(y) * Framebuffer::PITCH,
                    result.pitch);
    }
    SDL_UnlockMutex(mutex_);
    return result;
}

VDP::StateSnapshot VDP::stateSnapshot() const {
    StateSnapshot result;
    SDL_LockMutex(mutex_);
    std::copy(std::begin(state_.regs_), std::end(state_.regs_), result.regs.begin());
    std::copy(std::begin(state_.vram_), std::end(state_.vram_), result.vram.begin());
    std::copy(std::begin(state_.cram_), std::end(state_.cram_), result.cram.begin());
    std::copy(std::begin(state_.vsram_), std::end(state_.vsram_), result.vsram.begin());
    std::copy(std::begin(state_.sat_), std::end(state_.sat_), result.sat.begin());
    result.status = state_.status_;
    result.hCounter = state_.hCounter_;
    result.vCounter = state_.vCounter_;
    result.activeWidth = static_cast<std::uint16_t>(state_.activeWidth());
    result.activeHeight = static_cast<std::uint16_t>(state_.activeHeight());
    result.outputHeight = static_cast<std::uint16_t>(state_.activeOutputHeight());
    result.planeWidthCells = static_cast<std::uint16_t>(state_.planeWidthCells());
    result.planeHeightCells = static_cast<std::uint16_t>(state_.planeHeightCells());
    result.planeABase = static_cast<std::uint16_t>(state_.planeABase());
    result.planeBBase = static_cast<std::uint16_t>(state_.planeBBase());
    result.windowBase = static_cast<std::uint16_t>(state_.windowBase());
    result.windowWidthCells = static_cast<std::uint16_t>(state_.h40Mode() ? 64 : 32);
    result.satBase = static_cast<std::uint16_t>(state_.satBase());
    SDL_UnlockMutex(mutex_);
    return result;
}

bool VDP::waitForVSyncCount(std::uint32_t count, std::uint32_t timeoutMs) {
    if (count == 0)
        return true;
    std::unique_lock lock(syncEventMutex_);
    const auto target = vSyncGeneration_ + count;
    const auto wake = syncWakeGeneration_;
    return syncEventCV_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        return vSyncGeneration_ >= target || syncWakeGeneration_ != wake || !running_.load(std::memory_order_acquire);
    }) && vSyncGeneration_ >= target;
}

bool VDP::waitForHSyncCount(std::uint32_t count, std::uint32_t timeoutMs) {
    if (count == 0)
        return true;
    std::unique_lock lock(syncEventMutex_);
    const auto target = hSyncGeneration_ + count;
    const auto wake = syncWakeGeneration_;
    return syncEventCV_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        return hSyncGeneration_ >= target || syncWakeGeneration_ != wake || !running_.load(std::memory_order_acquire);
    }) && hSyncGeneration_ >= target;
}

bool VDP::waitForHSyncLine(std::uint16_t line, std::uint32_t timeoutMs) {
    if (line >= hSyncLineGenerations_.size())
        return false;
    std::unique_lock lock(syncEventMutex_);
    const auto initial = hSyncLineGenerations_[line];
    const auto wake = syncWakeGeneration_;
    return syncEventCV_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        return hSyncLineGenerations_[line] != initial || syncWakeGeneration_ != wake ||
               !running_.load(std::memory_order_acquire);
    }) && hSyncLineGenerations_[line] != initial;
}

void VDP::wakeSyncWaiters() {
    {
        std::lock_guard lock(syncEventMutex_);
        ++syncWakeGeneration_;
    }
    syncEventCV_.notify_all();
}

void VDP::signalHSync(int line) {
    {
        std::lock_guard lock(syncEventMutex_);
        ++hSyncGeneration_;
        if (line >= 0 && line < static_cast<int>(hSyncLineGenerations_.size()))
            ++hSyncLineGenerations_[static_cast<std::size_t>(line)];
    }
    syncEventCV_.notify_all();
}

void VDP::signalVSync() {
    {
        std::lock_guard lock(syncEventMutex_);
        ++vSyncGeneration_;
    }
    if (env_ != nullptr)
        env_->recordVSync();
    syncEventCV_.notify_all();
}

// ── Port I/O ─────────────────────────────────────────────────────────────────

/// Thread-safe: Acquires mutex_, delegates to port_.writeControlPort(), releases mutex_.
void VDP::writeControlPort(m_word value) {
    SDL_LockMutex(mutex_);
    port_.writeControlPort(value);
    SDL_UnlockMutex(mutex_);
}

/// Thread-safe: Acquires mutex_, reads status from port_, releases mutex_.
m_word VDP::readControlPort() {
    SDL_LockMutex(mutex_);
    m_word result = port_.readControlPort();
    SDL_UnlockMutex(mutex_);
    return result;
}

/// Thread-safe: Acquires mutex_, delegates to port_.writeDataPort(), releases mutex_.
void VDP::writeDataPort(m_word value) {
    SDL_LockMutex(mutex_);
    port_.writeDataPort(value);
    SDL_UnlockMutex(mutex_);
}

/// Thread-safe: Acquires mutex_, reads from port_, releases mutex_.
m_word VDP::readDataPort() {
    SDL_LockMutex(mutex_);
    m_word result = port_.readDataPort();
    SDL_UnlockMutex(mutex_);
    return result;
}

/// Thread-safe: Acquires mutex_, reads H/V counter, releases mutex_.
m_word VDP::readHVCounter() {
    SDL_LockMutex(mutex_);
    m_word result = port_.readHVCounter();
    SDL_UnlockMutex(mutex_);
    return result;
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

/// Thread-safe: Resets VDP state and clears framebuffer under mutex protection.
void VDP::reset() {
    SDL_LockMutex(mutex_);
    state_.reset();
    framebuffer_.clear();
    SDL_UnlockMutex(mutex_);
    SDL_LockMutex(irqMutex_);
    irqQueue_.clear();
    SDL_UnlockMutex(irqMutex_);
}

/// Signals render thread to exit, waits for completion, then releases all SDL resources.
void VDP::shutdown() {
    stop();
    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    if (sdlRenderer_) {
        SDL_DestroyRenderer(sdlRenderer_);
        sdlRenderer_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    if (mutex_) {
        SDL_DestroyMutex(mutex_);
        mutex_ = nullptr;
    }
    if (irqMutex_) {
        SDL_DestroyMutex(irqMutex_);
        irqMutex_ = nullptr;
    }
}

// ── Interrupt scheduling ───────────────────────────────────────────────────

/// Appends an interrupt to the queue, dropping the oldest entries past IRQ_QUEUE_MAX.
void VDP::scheduleInterrupt(Interrupt::Type type, int line) {
    SDL_LockMutex(irqMutex_);
    if (irqQueue_.size() >= IRQ_QUEUE_MAX) {
        irqQueue_.pop_front();
    }
    irqQueue_.push_back(Interrupt{type, line});
    SDL_UnlockMutex(irqMutex_);

    // Also raise the lock-free pending-interrupt flag consulted by recompiled
    // code before each instruction (the queue above drives the cooperative
    // hSync()/vSync() path used by hand-written programs). VBlank is autovector
    // level 6, HBlank level 4. Unlike the queue, this honours the VDP's own
    // interrupt-enable bits, exactly as the 68000 IRQ line does on hardware: a
    // VBlank IRQ is only asserted when reg $01 bit 5 (IE0) is set, so the game's
    // handler never runs before the game has enabled it (and set up its data).
    // HBlank is already gated by the caller (hintEnabled()).
    if (env_) {
        if (type == Interrupt::VSync) {
            SDL_LockMutex(mutex_);
            bool vintOn = state_.vblankIRQEnabled();
            SDL_UnlockMutex(mutex_);
            if (vintOn) {
                env_->z80().pulseVBlankIRQ();
                env_->raiseInterrupt(6);
            }
        } else {
            env_->raiseInterrupt(4);
        }
    }
}

/// Pops the oldest scheduled interrupt. Returns false when the queue is empty.
bool VDP::popInterrupt(Interrupt &out) {
    SDL_LockMutex(irqMutex_);
    bool has = !irqQueue_.empty();
    if (has) {
        out = irqQueue_.front();
        irqQueue_.pop_front();
    }
    SDL_UnlockMutex(irqMutex_);
    return has;
}

// ── Debug Dump ───────────────────────────────────────────────────────────────

/// Thread-safe: Exports framebuffer to PNG under mutex. fullRange=true uses 8-bit color; false uses native 3-bit.
void VDP::dumpFrameBufferToPNG(std::string path, bool fullRange) {
    SDL_LockMutex(mutex_);
    rendererDebug_.dumpFrameBufferToPNG(path, fullRange);
    SDL_UnlockMutex(mutex_);
}

/// Thread-safe: Exports combined debug image (framebuffer + tiles + planes) to PNG under mutex.
void VDP::dumpEverythingToPNG(std::string path, bool fullRange) {
    SDL_LockMutex(mutex_);
    rendererDebug_.dumpEverythingToPNG(path, fullRange);
    SDL_UnlockMutex(mutex_);
}

// ── Render Thread ────────────────────────────────────────────────────────────

/// Static thread entry point. Casts userdata to VDP* and delegates to renderLoop().
int VDP::renderThreadEntry(void *data) {
    return static_cast<VDP *>(data)->renderLoop();
}

/// SDL main-thread callback to update and present framebuffer texture to screen.
void VDP::sdlPresentCallback(void *userdata) {
    static_cast<VDP *>(userdata)->presentToScreen();
}

/// SDL main-thread callback to re-present the same texture (for VSync2/VSync3 frame hold).
void VDP::sdlRepeatCallback(void *userdata) {
    auto *self = static_cast<VDP *>(userdata);
    if (self->sdlRenderer_ && self->texture_) {
        self->presentToScreen();
    }
}

/// Uploads framebuffer to the long-lived texture and presents to window (must be called on SDL main thread).
void VDP::presentToScreen() {
    if (!texture_)
        return;
    updateWindowTitle();
    framebuffer_.uploadToTexture(texture_);

    m_byte bgR, bgG, bgB;
    tile_.cramToRGB_FullRange(static_cast<m_byte>(state_.bgColorPalette()),
                              static_cast<m_byte>(state_.bgColorIndex()),
                              bgR,
                              bgG,
                              bgB);
    SDL_SetRenderDrawColor(sdlRenderer_, bgR, bgG, bgB, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(sdlRenderer_);

    int outputW = 0;
    int outputH = 0;
    if (!SDL_GetRenderOutputSize(sdlRenderer_, &outputW, &outputH) || outputW <= 0 || outputH <= 0)
        return;

    const int framebufferW = state_.activeWidth();
    const int framebufferH = state_.activeOutputHeight();
    const float scale = std::min(static_cast<float>(outputW) / static_cast<float>(framebufferW),
                                 static_cast<float>(outputH) / static_cast<float>(framebufferH));
    const float viewportW = static_cast<float>(framebufferW) * scale;
    const float viewportH = static_cast<float>(framebufferH) * scale;

    SDL_FRect src{0.0f,
                  0.0f,
                  static_cast<float>(framebufferW),
                  static_cast<float>(framebufferH)};
    SDL_FRect dst{(static_cast<float>(outputW) - viewportW) * 0.5f,
                  (static_cast<float>(outputH) - viewportH) * 0.5f,
                  viewportW,
                  viewportH};
    SDL_RenderTexture(sdlRenderer_, texture_, &src, &dst);
    SDL_RenderPresent(sdlRenderer_);
}

void VDP::updateWindowTitle() {
    if (!window_)
        return;

    const uint64_t averageNs = averageFrameTimeNs_.load(std::memory_order_relaxed);
    if (averageNs == 0 || averageNs == displayedFrameTimeNs_)
        return;

    char title[64];
    std::snprintf(
        title, sizeof title, "VDP | avg 60 frames: %.3f ms", static_cast<double>(averageNs) / 1'000'000.0);
    SDL_SetWindowTitle(window_, title);
    displayedFrameTimeNs_ = averageNs;
}

/// Main render thread loop: renders the frame scanline by scanline (scheduling an HSync interrupt after each line),
/// sets the VBlank flag, schedules a VSync interrupt, presents to display, and manages frame timing based on sync mode.
/// Interrupts are dispatched on the program thread via MegaDriveEnvironment::runVDPInterrupts().
int VDP::renderLoop() {
    static constexpr size_t kFrameTimeWindow = 60;
    std::array<uint64_t, kFrameTimeWindow> frameTimes{};
    uint64_t frameTimeSum = 0;
    size_t frameTimeCount = 0;
    size_t frameTimeIndex = 0;
    size_t framesSinceTitleUpdate = 0;
    uint64_t nextFrameDeadline = SDL_GetTicksNS();

    while (running_) {
        const std::uint64_t frameFrequencyHz = env_ != nullptr ? env_->vdpInternalFrequencyHz() : 60u;
        const std::uint64_t frameTimeNs =
            std::max<std::uint64_t>(1u, (1'000'000'000ull + frameFrequencyHz / 2u) / frameFrequencyHz);
        uint64_t frameStart = SDL_GetTicksNS();

        // Render the frame one scanline at a time so a per-line interrupt can be
        // scheduled (raster effects). Mirrors VDPRenderer::renderFrame().
        SDL_LockMutex(mutex_);
        bool displayEnabled = state_.displayEnabled();
        const int activeLines = state_.activeHeight();
        if (!displayEnabled) {
            framebuffer_.clear();
        }
        SDL_UnlockMutex(mutex_);

        if (displayEnabled) {
            // ponytail: single lock for the full frame instead of per-scanline;
            // reduces ~448 mutex ops to 1. CPU thread is blocked from VDP writes
            // during active display, which mirrors real hardware behaviour.
            // scheduleInterrupt(HSync) is safe under mutex_: it only touches
            // irqMutex_ + a raiseInterrupt atomic, never mutex_ itself.
            SDL_LockMutex(mutex_);
            framebuffer_.clear();
            state_.status_ &= ~0x0088; // clear VINT/VBlank at the start of active rendering
            if (state_.interlaced() && state_.oddFrame_)
                state_.status_ |= 0x0010;
            else
                state_.status_ &= ~0x0010;
            int hintCountdown = state_.hintReloadValue();
            for (int line = 0; line < activeLines; ++line) {
                state_.vCounter_ = static_cast<m_word>(line);
                renderer_.renderScanline(line);
                signalHSync(line);
                --hintCountdown;
                if (hintCountdown < 0) {
                    hintCountdown = state_.hintReloadValue();
                    if (state_.hintEnabled()) {
                        scheduleInterrupt(Interrupt::HSync, line);
                    }
                }
            }
            SDL_UnlockMutex(mutex_);
        } else {
            // Sync timing continues while display output is disabled.
            for (int line = 0; line < activeLines; ++line)
                signalHSync(line);
        }

        SDL_LockMutex(mutex_);
        state_.status_ |= 0x0088; // VBlank + VINT flags
        SDL_UnlockMutex(mutex_);

        scheduleInterrupt(Interrupt::VSync, 0);
        signalVSync();

        // Keep --debug lightweight: emit diagnostics once per second without
        // doing image encoding or filesystem I/O on the render thread. Debug
        // PNGs remain available explicitly through the screenshot hotkeys.
        if (env_ != nullptr && env_->debugLog()) {
            ++debugFrame_;
            const std::uint64_t now = SDL_GetTicksNS();
            if (now >= nextDebugLogNs_) {
                env_->logFrame(debugFrame_, displayEnabled);
                nextDebugLogNs_ = now + 1'000'000'000ull;
            }
        }

        if (sdlRenderer_) {
            SDL_RunOnMainThread(sdlPresentCallback, this, /*wait=*/true);

            // VSync2 / VSync3: hold the same frame for (N-1) extra monitor refreshes
            // so the game runs at monitorHz / N (e.g. 120 Hz / 2 = 60 Hz).
            int extraPresentCount = 0;
            if (syncMode_ == VSync2)
                extraPresentCount = 1;
            else if (syncMode_ == VSync3)
                extraPresentCount = 2;
            for (int i = 0; i < extraPresentCount; ++i) {
                SDL_RunOnMainThread(sdlRepeatCallback, this, /*wait=*/true);
            }
        }

        SDL_LockMutex(mutex_);
        state_.status_ &= ~0x0008;
        state_.vCounter_ = 0;
        if (state_.interlaced()) {
            state_.oddFrame_ = !state_.oddFrame_;
        } else {
            state_.oddFrame_ = false;
        }
        SDL_UnlockMutex(mutex_);

        if (syncMode_ == InternalTimer) {
            nextFrameDeadline += frameTimeNs;
            const uint64_t now = SDL_GetTicksNS();
            if (now < nextFrameDeadline) {
                SDL_DelayPrecise(nextFrameDeadline - now);
            } else if (now - nextFrameDeadline >= frameTimeNs) {
                // Do not burst through several frames after a debugger pause,
                // debug dump, or host stall. Re-anchor while retaining normal
                // sub-frame error correction.
                nextFrameDeadline = now;
            }
        }

        const uint64_t completedFrameTime = SDL_GetTicksNS() - frameStart;
        if (frameTimeCount < kFrameTimeWindow) {
            ++frameTimeCount;
        } else {
            frameTimeSum -= frameTimes[frameTimeIndex];
        }
        frameTimes[frameTimeIndex] = completedFrameTime;
        frameTimeSum += completedFrameTime;
        frameTimeIndex = (frameTimeIndex + 1) % kFrameTimeWindow;
        ++framesSinceTitleUpdate;
        if (frameTimeCount == kFrameTimeWindow && framesSinceTitleUpdate == kFrameTimeWindow) {
            averageFrameTimeNs_.store(frameTimeSum / kFrameTimeWindow, std::memory_order_relaxed);
            framesSinceTitleUpdate = 0;
        }
    }

    return 0;
}
