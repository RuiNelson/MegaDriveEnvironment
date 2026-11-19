/**
 * @file VDP.cpp
 * @brief Sega Mega Drive VDP emulator — composite implementation.
 */

#include "VDP.hpp"
#include "VDPPresenter.hpp"
#include "system/MegaDriveEnvironment.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <png.h>
#include <utility>

// ── Construction / Destruction ──────────────────────────────────────────────

/// Initializes SDL, creates window and renderer, and resets VDP state. The
/// render thread is not spawned here — call start() once the environment is
/// fully constructed.
VDP::VDP(MegaDriveEnvironment *env, Synchronization synchronization, Scaling scaling, SpriteLimitMode spriteLimitMode)
    : state_(), tile_(state_), port_(state_), renderer_(state_, tile_, framebuffer_),
      rendererDebug_(state_, tile_, framebuffer_), syncMode_(synchronization), scalingMode_(scaling), env_(env) {
    SDL_Init(SDL_INIT_VIDEO);
    mutex_    = SDL_CreateMutex();
    layers_        = std::make_unique<VDPFrameLayers>();
    presentLayers_ = std::make_unique<VDPFrameLayers>();
    shownLayers_   = std::make_unique<VDPFrameLayers>();
    irqMutex_ = SDL_CreateMutex();
    port_.setEnvironment(env);
    renderer_.setHardwareSpriteLimits(spriteLimitMode == HardwareSpriteLimit);
    state_.reset();

    // macOS presents through Metal directly (VDPPresenterMetal.mm) when the video driver is Cocoa, so
    // Metal-only work can be added there; any other driver (the tests' `dummy`) and every other platform
    // use SDL_Renderer.
    SDL_WindowFlags windowFlags = SDL_WINDOW_RESIZABLE;
#ifdef __APPLE__
    const char *videoDriver = SDL_GetCurrentVideoDriver();
    const bool  metal       = videoDriver != nullptr && std::strcmp(videoDriver, "cocoa") == 0;
    if (metal) // the display's real pixels, so the scaling filter is the only one (no 2x by the system)
        windowFlags |= SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
    // Sized once it exists: the size depends on the borders, which only a window has.
    window_ = SDL_CreateWindow("VDP", VDPState::SCREEN_W, VDPState::SCREEN_H, windowFlags);
    if (window_) {
        fitWindow();
#ifdef __APPLE__
        if (metal)
            presenter_ = createMetalPresenter(window_);
#endif
        if (!presenter_)
            presenter_ = createSDLPresenter(window_);
        if (presenter_)
            SDL_Log("[VDP] Presenting through %s", presenter_->name());
        // In front of whatever launched it: macOS does not reliably activate a process started from a
        // terminal or another app's window.
        SDL_RaiseWindow(window_);
    }
}

/// Picks the window size for the current scaling mode: the active picture (320×224, or 384×216 in Mode 48)
/// times the fixed factor, or the largest integer factor that fits the usable display area with the window's
/// borders: a window that does not fit is cut by the system, and no longer has the picture's proportions.
void VDP::windowSize(int &winW, int &winH) const {
    const int baseW = widescreen() ? VDPState::WIDESCREEN_W : VDPState::SCREEN_W;
    const int baseH = widescreen() ? VDPState::WIDESCREEN_H : VDPState::SCREEN_H;
    winW            = baseW;
    winH            = baseH;
    if (scalingMode_ > 0) {
        winW *= scalingMode_;
        winH *= scalingMode_;
    } else if (scalingMode_ == Integer) {
        SDL_Rect      usable{};
        SDL_DisplayID display = SDL_GetPrimaryDisplay();
        if (display && SDL_GetDisplayUsableBounds(display, &usable)) {
            int top = 0, left = 0, bottom = 0, right = 0;
            if (window_) {
                SDL_GetWindowBordersSize(window_, &top, &left, &bottom, &right);
            }
            int scaleX = (usable.w - left - right) / baseW;
            int scaleY = (usable.h - top - bottom) / baseH;
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
}

void VDP::fitWindow() {
    int winW = 0;
    int winH = 0;
    windowSize(winW, winH);
    SDL_SetWindowSize(window_, winW, winH);
    SDL_SyncWindow(window_);
    if (scalingMode_ == Integer) {
        // macOS does not report the borders (SDL_GetWindowBordersSize fails), but it does cut a visible window
        // that does not fit with them (a hidden one keeps the size asked): one factor down until the window is
        // the size it was asked to be.
        const int baseW = widescreen() ? VDPState::WIDESCREEN_W : VDPState::SCREEN_W;
        const int baseH = widescreen() ? VDPState::WIDESCREEN_H : VDPState::SCREEN_H;
        int       gotW  = winW;
        int       gotH  = winH;
        SDL_GetWindowSize(window_, &gotW, &gotH);
        while ((gotW != winW || gotH != winH) && winW > baseW) {
            winW -= baseW;
            winH -= baseH;
            SDL_SetWindowSize(window_, winW, winH);
            SDL_SyncWindow(window_);
            SDL_GetWindowSize(window_, &gotW, &gotH);
        }
    }
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void VDP::setWidescreen(bool enabled) {
    SDL_LockMutex(mutex_);
    const bool changed = state_.widescreen_ != enabled;
    state_.widescreen_ = enabled;
    SDL_UnlockMutex(mutex_);
    if (window_ && changed) {
        // A fullscreen window has no size of its own to fit, and would read as cut by the system.
        if (fullscreen()) {
            fitOnLeavingFullscreen_ = true;
        } else {
            fitWindow();
        }
    }
}

bool VDP::widescreen() const {
    return state_.widescreen_;
}

void VDP::setIntegerScaling(bool enabled) {
    integerScaling_.store(enabled, std::memory_order_relaxed);
}

bool VDP::integerScaling() const {
    return integerScaling_.load(std::memory_order_relaxed);
}

void VDP::setScalingFilter(ScalingFilter filter) {
    scalingFilter_.store(filter, std::memory_order_relaxed);
}

VDP::ScalingFilter VDP::scalingFilter() const {
    return scalingFilter_.load(std::memory_order_relaxed);
}

void VDP::setFrameGeneration(FrameGeneration mode) {
    frameGeneration_.store(mode, std::memory_order_relaxed);
}

VDP::FrameGeneration VDP::frameGeneration() const {
    return frameGeneration_.load(std::memory_order_relaxed);
}

bool VDP::frameGenerationSupported() const {
    return presenter_ && presenter_->supportsFrameGeneration();
}

bool VDP::generatingFrames() const {
    if (frameGeneration() == NoFrameGeneration || !frameGenerationSupported() || syncMode_ != InternalTimer || state_.interlaced())
        return false;
    // At the normal rate only: turbo is a development mode.
    return env_ == nullptr || env_->vdpInternalFrequencyHz() <= 60u;
}

bool VDP::scalingFilterSupported(ScalingFilter filter) const {
    if (filter != MetalFX)
        return true;
    return presenter_ && presenter_->supports(filter);
}

int VDP::hybridFactor(float targetSize, int pictureSize) {
    if (pictureSize <= 0)
        return 1;
    // A hair under a whole multiple is that multiple: the rectangle is computed in floats.
    return std::max(1, static_cast<int>(std::ceil(targetSize / static_cast<float>(pictureSize) - 1e-3f)));
}

VDP::~VDP() {
    shutdown();
}

bool VDP::setFullscreen(bool fullscreen) {
    return window_ && SDL_SetWindowFullscreen(window_, fullscreen);
}

bool VDP::fullscreen() const {
    return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
}

void VDP::applyLive(const LiveSettings &settings) {
    syncMode_.store(settings.synchronization, std::memory_order_relaxed);
    struct Call {
        VDP               *vdp;
        const LiveSettings &settings;
    } call{this, settings};
    SDL_RunOnMainThread(
        [](void *userdata) {
            const Call &c = *static_cast<Call *>(userdata);
            VDP        &v = *c.vdp;
            v.setIntegerScaling(c.settings.integerScaling);
            v.setScalingFilter(c.settings.scalingFilter);
            v.setFrameGeneration(c.settings.frameGeneration);
            // Windowed first, so a widescreen change sizes the window it will stay in.
            if (!c.settings.fullscreen && v.fullscreen() && v.setFullscreen(false)) {
                SDL_SyncWindow(v.window_);
            }
            v.setWidescreen(c.settings.widescreen);
            if (c.settings.fullscreen && !v.fullscreen() && v.setFullscreen(true)) {
                SDL_SyncWindow(v.window_);
            }
        },
        &call,
        /*wait_complete=*/true);
}

VDP::Synchronization VDP::synchronization() const {
    return syncMode_.load(std::memory_order_relaxed);
}

int VDP::displayRefreshHz() const {
    if (!window_) {
        return 0;
    }
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
    return mode ? static_cast<int>(std::lround(mode->refresh_rate)) : 0;
}

void VDP::setFrameOverlay(std::function<void(Framebuffer &, int, int)> overlay) {
    frameOverlay_ = std::move(overlay);
}

void VDP::fitAfterFullscreen() {
    if (fitOnLeavingFullscreen_ && window_) {
        fitOnLeavingFullscreen_ = false;
        fitWindow();
    }
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
    interruptDispatchCV_.notify_all();
    setRemoteLockstep(false, 1);
    wakeSyncWaiters();
    if (thread_) {
        SDL_Event e;
        while (SDL_GetThreadState(thread_) != SDL_THREAD_COMPLETE) {
            SDL_PollEvent(&e);
            SDL_Delay(1);
        }
        SDL_WaitThread(thread_, nullptr);
        thread_ = nullptr;
        // An asynchronous present still queued on the main thread refers to
        // this VDP; let it run (bounded, in case this is not the main thread).
        for (int i = 0; i < 1000 && presentPending_.load(std::memory_order_acquire); ++i) {
            SDL_PollEvent(&e);
            SDL_Delay(1);
        }
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

bool VDP::setRemoteLockstep(bool enabled, std::uint32_t timeoutMs) {
    if (!enabled) {
        {
            std::lock_guard lock(remoteLockstepMutex_);
            remoteLockstepRequested_ = false;
            remoteLockstepEngaged_ = false;
            remoteLockstepRendererPaused_ = false;
            remoteLockstepCPUPaused_ = false;
        }
        remoteLockstepCV_.notify_all();
        if (env_ != nullptr) {
            env_->interruptGeneration_.fetch_add(1, std::memory_order_release);
            env_->interruptGeneration_.notify_all();
        }
        return true;
    }
    if (timeoutMs == 0 || !running_.load(std::memory_order_acquire) || env_ == nullptr)
        return false;

    {
        std::lock_guard lock(remoteLockstepMutex_);
        remoteLockstepAllowedFrame_ = env_->gameUptimeFrames();
        remoteLockstepRequested_ = true;
    }
    remoteLockstepCV_.notify_all();
    env_->interruptGeneration_.fetch_add(1, std::memory_order_release);
    env_->interruptGeneration_.notify_all();

    std::unique_lock lock(remoteLockstepMutex_);
    const bool ready = remoteLockstepCV_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        const bool cpuActive = env_->bootRunning_.load(std::memory_order_acquire) &&
                               !env_->cpuDone_.load(std::memory_order_acquire);
        return !running_.load(std::memory_order_acquire) ||
               (remoteLockstepRequested_ && remoteLockstepEngaged_ &&
                remoteLockstepRendererPaused_ && (!cpuActive || remoteLockstepCPUPaused_));
    }) && running_.load(std::memory_order_acquire) && remoteLockstepRequested_ &&
           remoteLockstepEngaged_ && remoteLockstepRendererPaused_;
    lock.unlock();
    if (!ready)
        setRemoteLockstep(false, 1);
    return ready;
}

bool VDP::remoteLockstepEnabled() const {
    std::lock_guard lock(remoteLockstepMutex_);
    return remoteLockstepRequested_ && remoteLockstepEngaged_;
}

bool VDP::advanceRemoteLockstepFrame(std::uint32_t timeoutMs) {
    if (timeoutMs == 0 || env_ == nullptr)
        return false;

    std::unique_lock lock(remoteLockstepMutex_);
    const bool cpuActive = env_->bootRunning_.load(std::memory_order_acquire) &&
                           !env_->cpuDone_.load(std::memory_order_acquire);
    if (!running_.load(std::memory_order_acquire) || !remoteLockstepRequested_ ||
        !remoteLockstepEngaged_ || !remoteLockstepRendererPaused_ ||
        (cpuActive && !remoteLockstepCPUPaused_))
        return false;

    const std::uint64_t target = env_->gameUptimeFrames() + 1;
    remoteLockstepAllowedFrame_ = target;
    remoteLockstepRendererPaused_ = false;
    remoteLockstepCPUPaused_ = false;
    lock.unlock();
    remoteLockstepCV_.notify_all();
    env_->interruptGeneration_.fetch_add(1, std::memory_order_release);
    env_->interruptGeneration_.notify_all();
    lock.lock();

    return remoteLockstepCV_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        const bool activeCPU = env_->bootRunning_.load(std::memory_order_acquire) &&
                               !env_->cpuDone_.load(std::memory_order_acquire);
        return !running_.load(std::memory_order_acquire) || !remoteLockstepRequested_ ||
               (env_->gameUptimeFrames() >= target && remoteLockstepRendererPaused_ &&
                (!activeCPU || remoteLockstepCPUPaused_));
    }) && running_.load(std::memory_order_acquire) && remoteLockstepRequested_ &&
           env_->gameUptimeFrames() == target && remoteLockstepRendererPaused_;
}

void VDP::remoteLockstepCPUCheckpoint() {
    if (!remoteLockstepRequested_)
        return;
    std::unique_lock lock(remoteLockstepMutex_);
    if (!remoteLockstepRequested_)
        return;
    remoteLockstepCPUPaused_ = true;
    remoteLockstepCV_.notify_all();
    remoteLockstepCV_.wait(lock, [&] {
        return !remoteLockstepRequested_ || !running_.load(std::memory_order_acquire) ||
               env_ == nullptr || env_->shouldQuit() ||
               env_->restartRequested_.load(std::memory_order_acquire) ||
               env_->gameUptimeFrames() < remoteLockstepAllowedFrame_;
    });
    remoteLockstepCPUPaused_ = false;
    remoteLockstepCV_.notify_all();
}

bool VDP::remoteLockstepRenderCheckpoint() {
    std::unique_lock lock(remoteLockstepMutex_);
    if (!remoteLockstepRequested_)
        return running_.load(std::memory_order_acquire);

    remoteLockstepEngaged_ = true;
    remoteLockstepRendererPaused_ = true;
    remoteLockstepCV_.notify_all();
    remoteLockstepCV_.wait(lock, [&] {
        return !remoteLockstepRequested_ || !running_.load(std::memory_order_acquire) ||
               env_ == nullptr || env_->shouldQuit() ||
               env_->restartRequested_.load(std::memory_order_acquire) ||
               env_->gameUptimeFrames() < remoteLockstepAllowedFrame_;
    });
    remoteLockstepRendererPaused_ = false;
    remoteLockstepCV_.notify_all();
    return running_.load(std::memory_order_acquire);
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
    const bool displayWasEnabled = state_.displayEnabled();
    port_.writeControlPort(value);
    // A real VDP blanks its output as soon as register 1 clears DISP. The
    // renderer may already have completed the previous frame, so merely
    // changing the register would leave that frame in the host framebuffer
    // until the next render pass. Transitions in a native game commonly turn
    // DISP off and back on during one VBlank; clear at the falling edge so the
    // SDL presentation cannot expose the old scene in between.
    if (displayWasEnabled && !state_.displayEnabled()) {
        framebuffer_.clear();
    }
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
    {
        std::lock_guard lock(interruptDispatchMutex_);
        completedInterruptDispatchTicket_ = nextInterruptDispatchTicket_;
    }
    interruptDispatchCV_.notify_all();
}

/// Signals render thread to exit, waits for completion, then releases all SDL resources.
void VDP::shutdown() {
    stop();
    presenter_.reset();
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
    bool callbackEnabled = true;
    if (type == Interrupt::VSync) {
        SDL_LockMutex(mutex_);
        callbackEnabled = state_.vblankIRQEnabled();
        SDL_UnlockMutex(mutex_);
    }

    std::uint64_t dispatchTicket = 0;
    if (callbackEnabled) {
        std::lock_guard lock(interruptDispatchMutex_);
        dispatchTicket = ++nextInterruptDispatchTicket_;

        SDL_LockMutex(irqMutex_);
        if (irqQueue_.size() >= IRQ_QUEUE_MAX) {
            irqQueue_.pop_front();
        }
        irqQueue_.push_back(Interrupt{type, line, dispatchTicket});
        SDL_UnlockMutex(irqMutex_);
        if (env_) {
            env_->notifyVDPCallbackScheduled();
        }
    }

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
            if (callbackEnabled) {
                env_->z80().pulseVBlankIRQ();
                env_->raiseInterrupt(6);
            }
        } else {
            env_->raiseInterrupt(4);
        }
    }

    if (dispatchTicket != 0) {
        std::unique_lock lock(interruptDispatchMutex_);
        interruptDispatchCV_.wait(lock, [&] {
            return completedInterruptDispatchTicket_ >= dispatchTicket ||
                   !running_.load(std::memory_order_acquire);
        });
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

void VDP::discardPendingInterrupts() {
    Interrupt interrupt;
    while (popInterrupt(interrupt)) {
        acknowledgeInterrupt(interrupt);
    }
}

void VDP::acknowledgeInterrupt(const Interrupt &interrupt) {
    if (interrupt.dispatchTicket == 0) {
        return;
    }
    {
        std::lock_guard lock(interruptDispatchMutex_);
        completedInterruptDispatchTicket_ =
            std::max(completedInterruptDispatchTicket_, interrupt.dispatchTicket);
    }
    interruptDispatchCV_.notify_all();
}

void VDP::acknowledgeInterruptLevel(int level) {
    if (level != 4 && level != 6) {
        return;
    }
    {
        std::lock_guard lock(interruptDispatchMutex_);
        completedInterruptDispatchTicket_ = nextInterruptDispatchTicket_;
    }
    interruptDispatchCV_.notify_all();
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
    if (self->presenter_) {
        self->presentToScreen();
    }
}

/// Copies the finished frame for the main thread and queues its presentation without waiting. Drops the frame if
/// the previous one is still queued, so the render thread runs at the internal timer's rate whatever the display does.
void VDP::queueAsyncPresent(bool generating) {
    if (presentPending_.load(std::memory_order_acquire))
        return;
    SDL_LockMutex(mutex_);
    const int width  = state_.activeWidth();
    const int height = std::clamp(state_.activeOutputHeight(), 0, Framebuffer::HEIGHT);
    const size_t bytes = static_cast<size_t>(height) * Framebuffer::PITCH;
    // Generating frames, a picture that repeats the one on screen is not presented: the interpolation runs
    // between pictures that differ, as far apart as the game makes them (every other frame is common).
    if (generating && presentLayersValid_ && width == presentWidth_ && height == presentHeight_ &&
        std::memcmp(presentFramebuffer_.getRawPointer(), framebuffer_.getRawPointer(), bytes) == 0) {
        SDL_UnlockMutex(mutex_);
        return;
    }
    presentWidth_  = width;
    presentHeight_ = height;
    std::memcpy(presentFramebuffer_.getRawPointer(), framebuffer_.getRawPointer(), bytes);
    presentLayersValid_ = generating;
    if (generating)
        *presentLayers_ = *layers_;
    presentTimeNs_ = SDL_GetTicksNS();
    tile_.cramToRGB_FullRange(static_cast<m_byte>(state_.bgColorPalette()),
                              static_cast<m_byte>(state_.bgColorIndex()),
                              presentBg_[0],
                              presentBg_[1],
                              presentBg_[2]);
    SDL_UnlockMutex(mutex_);
    presentPending_.store(true, std::memory_order_release);
    if (!SDL_RunOnMainThread(sdlAsyncPresentCallback, this, /*wait=*/false))
        presentPending_.store(false, std::memory_order_release);
}

/// SDL main-thread callback to present the frame copied by queueAsyncPresent().
void VDP::sdlAsyncPresentCallback(void *userdata) {
    auto *self = static_cast<VDP *>(userdata);
    if (self->presenter_) {
        self->updateWindowTitle();
        self->presenter_->upload(self->presentFramebuffer_, self->presentWidth_, self->presentHeight_);
        VDPFrameGeneration generation;
        if (self->presentLayersValid_) {
            generation.keep = true;
            // Interpolate when the display can show a frame between the two: two refreshes or more apart,
            // and not after a pause, a menu that waited, or a picture of another size.
            const std::uint64_t interval = self->presentTimeNs_ - self->shownTimeNs_;
            const int           hz       = self->displayRefreshHz();
            const std::uint64_t refresh  = 1'000'000'000ull / static_cast<std::uint64_t>(hz > 0 ? hz : 60);
            if (self->shownLayersValid_ && interval * 10 >= refresh * 18 && interval <= 100'000'000ull) {
                self->motion_.resize(static_cast<size_t>(self->presentWidth_) * self->presentHeight_ * 2);
                computeMotion(*self->shownLayers_, *self->presentLayers_, self->motion_.data());
                generation.motion   = self->motion_.data();
                generation.interval = static_cast<double>(interval) / 1e9;
                if (self->frameGeneration() == GenerateHoldPoses) {
                    self->holds_.resize(static_cast<size_t>(self->presentWidth_) * self->presentHeight_ * 4);
                    if (computePoseHolds(*self->shownLayers_, *self->presentLayers_, self->holds_.data()))
                        generation.holds = self->holds_.data();
                }
            }
            std::swap(self->shownLayers_, self->presentLayers_);
            self->shownTimeNs_ = self->presentTimeNs_;
        }
        self->shownLayersValid_ = self->presentLayersValid_;
        self->renderTexture(self->presentWidth_,
                            self->presentHeight_,
                            self->presentBg_[0],
                            self->presentBg_[1],
                            self->presentBg_[2],
                            &generation);
    }
    self->presentPending_.store(false, std::memory_order_release);
}

/// Uploads framebuffer to the long-lived texture and presents to window (must be called on SDL main thread).
void VDP::presentToScreen() {
    if (!presenter_)
        return;
    updateWindowTitle();
    int framebufferW;
    int framebufferH;
    m_byte bgR;
    m_byte bgG;
    m_byte bgB;
    {
        // The CPU can rebuild VRAM/CRAM and change the display registers while
        // the SDL main thread is presenting. Keep the framebuffer upload and
        // the matching dimensions/background colour as one coherent snapshot.
        SDL_LockMutex(mutex_);
        framebufferW = state_.activeWidth();
        framebufferH = state_.activeOutputHeight();
        presenter_->upload(framebuffer_, framebufferW, framebufferH);
        tile_.cramToRGB_FullRange(static_cast<m_byte>(state_.bgColorPalette()),
                                  static_cast<m_byte>(state_.bgColorIndex()),
                                  bgR,
                                  bgG,
                                  bgB);
        SDL_UnlockMutex(mutex_);
    }
    renderTexture(framebufferW, framebufferH, bgR, bgG, bgB);
}

/// Clears to the background colour and draws the uploaded W×H area, centred and scaled to fit (main thread).
void VDP::renderTexture(int framebufferW, int framebufferH, m_byte bgR, m_byte bgG, m_byte bgB, const VDPFrameGeneration *generation) {
    const m_byte  background[3] = {bgR, bgG, bgB};
    ScalingFilter filter        = scalingFilter();
    if (!presenter_->supports(filter))
        filter = Hybrid;
    // The synchronous path (the VSync modes, the first frame) keeps nothing for frame generation.
    shownLayersValid_ = shownLayersValid_ && generation != nullptr;
    presenter_->present(framebufferW, framebufferH, background, integerScaling(), filter, generation ? *generation : VDPFrameGeneration{});
}

SDL_FRect VDP::presentationRect(int outputW, int outputH, int framebufferW, int framebufferH, bool integerScaling) {
    float scale = std::min(static_cast<float>(outputW) / static_cast<float>(framebufferW),
                           static_cast<float>(outputH) / static_cast<float>(framebufferH));
    if (integerScaling) {
        const int whole = std::min(outputW / framebufferW, outputH / framebufferH);
        if (whole >= 1)
            scale = static_cast<float>(whole);
    }
    const float viewportW = static_cast<float>(framebufferW) * scale;
    const float viewportH = static_cast<float>(framebufferH) * scale;
    // Whole-pixel origin so integer-scaled pixels map onto output pixels exactly.
    return SDL_FRect{std::floor((static_cast<float>(outputW) - viewportW) * 0.5f),
                     std::floor((static_cast<float>(outputH) - viewportH) * 0.5f),
                     viewportW,
                     viewportH};
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
/// Interrupts are dispatched automatically on the CPU thread by the environment.
int VDP::renderLoop() {
    static constexpr size_t kFrameTimeWindow = 60;
    std::array<uint64_t, kFrameTimeWindow> frameTimes{};
    uint64_t frameTimeSum = 0;
    size_t frameTimeCount = 0;
    size_t frameTimeIndex = 0;
    size_t framesSinceTitleUpdate = 0;
    uint64_t nextFrameDeadline = SDL_GetTicksNS();
    // The first frame is always presented synchronously: it holds the render
    // thread until boot() pumps events, which it does only once the CPU thread
    // exists, so no frame (and no VSync count) runs ahead of run().
    bool firstFrame = true;

    while (running_) {
        if (!remoteLockstepRenderCheckpoint())
            break;
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
        // Frame generation's record of where each pixel comes from (see VDPMotion.hpp).
        const bool generating = presenter_ != nullptr && generatingFrames();
        renderer_.setLayerRecord(generating ? layers_.get() : nullptr);
        if (generating) {
            layers_->begin(state_.activeWidth(),
                           state_.activeOutputHeight(),
                           state_.planeWidthCells() * 8,
                           state_.planeHeightCells() * 8,
                           state_.centerOffsetX());
            if (!displayEnabled)
                layers_->source.fill(VDPFrameLayers::Backdrop);
        }
        SDL_UnlockMutex(mutex_);

        if (displayEnabled) {
            SDL_LockMutex(mutex_);
            state_.status_ &= ~0x0088; // clear VINT/VBlank at the start of active rendering
            if (state_.interlaced() && state_.oddFrame_)
                state_.status_ |= 0x0010;
            else
                state_.status_ &= ~0x0010;
            int hintCountdown = state_.hintReloadValue();
            SDL_UnlockMutex(mutex_);

            // Keep the live VDP lock only for one scanline at a time. The 68000
            // can access the ports between lines instead of waiting for the
            // complete active frame, and raster writes remain visible to later
            // lines just as they are on hardware.
            for (int line = 0; line < activeLines; ++line) {
                SDL_LockMutex(mutex_);
                state_.vCounter_ = static_cast<m_word>(line);
                renderer_.renderScanline(line);
                --hintCountdown;
                const bool raiseHBlank = hintCountdown < 0 && state_.hintEnabled();
                if (hintCountdown < 0) {
                    hintCountdown = state_.hintReloadValue();
                }
                SDL_UnlockMutex(mutex_);

                signalHSync(line);
                if (raiseHBlank)
                    scheduleInterrupt(Interrupt::HSync, line);
            }
        } else {
            // Sync timing continues while display output is disabled.
            for (int line = 0; line < activeLines; ++line)
                signalHSync(line);
        }

        SDL_LockMutex(mutex_);
        if (displayEnabled && frameOverlay_) {
            const int width  = state_.activeWidth();
            const int height = std::clamp(state_.activeOutputHeight(), 0, Framebuffer::HEIGHT);
            const auto *pixels = static_cast<const m_byte *>(framebuffer_.getRawPointer());
            if (generating)
                beforeOverlay_.assign(pixels, pixels + static_cast<size_t>(height) * Framebuffer::PITCH);
            frameOverlay_(framebuffer_, width, height);
            // The host's drawing does not move with the layers under it.
            if (generating) {
                for (int y = 0; y < height; ++y) {
                    const m_byte *now    = pixels + static_cast<size_t>(y) * Framebuffer::PITCH;
                    const m_byte *before = beforeOverlay_.data() + static_cast<size_t>(y) * Framebuffer::PITCH;
                    if (std::memcmp(now, before, static_cast<size_t>(width) * Framebuffer::BPP) == 0)
                        continue;
                    for (int x = 0; x < width; ++x) {
                        if (std::memcmp(now + x * Framebuffer::BPP, before + x * Framebuffer::BPP, Framebuffer::BPP) != 0)
                            layers_->source[static_cast<size_t>(y) * VDPState::MAX_SCREEN_W + x] = VDPFrameLayers::Overlay;
                    }
                }
            }
        }
        state_.status_ |= 0x0088; // VBlank + VINT flags
        SDL_UnlockMutex(mutex_);

        // Queued remote presses change state at the frame boundary, before
        // the VBlank handler can sample the pads.
        if (env_ != nullptr)
            env_->controllers().advanceRemotePresses(env_->gameUptimeFrames() + 1);
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

        if (presenter_ && syncMode_ == InternalTimer && !firstFrame) {
            queueAsyncPresent(generating);
        } else if (presenter_) {
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

        firstFrame = false;

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
