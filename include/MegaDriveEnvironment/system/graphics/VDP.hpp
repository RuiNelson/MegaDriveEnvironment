#pragma once

#include "Framebuffer.hpp"
#include "VDPPort.hpp"
#include "VDPRenderer.hpp"
#include "VDPRendererDebug.hpp"
#include "VDPState.hpp"
#include "VDPTile.hpp"
#include <SDL3/SDL.h>
#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

class MegaDriveEnvironment;

/**
 * @file VDP.hpp
 * @brief Sega Mega Drive VDP emulator — public interface (composite root).
 *
 * Architecture:
 *
 *   ┌─────────────────────────────────────────────────────────┐
 *   │  Game thread (68k)                                      │
 *   │  writeControlPort / writeDataPort / readControlPort     │
 *   │         │                                                │
 *   │    ┌────▼────┐                                           │
 *   │    │  mutex_  │ ◄── all VDP state protected              │
 *   │    └────┬────┘                                           │
 *   │         │                                                │
 *   └─────────┼────────────────────────────────────────────────┘
 *             │
 *   ┌─────────▼────────────────────────────────────────────────┐
 *   │  Render thread                                           │
 *   │  renderLoop() → VDPRenderer → present to SDL window     │
 *   │  Calls environment->hSync(line) per scanline and         │
 *   │  environment->vSync() after each frame                   │
 *   └─────────────────────────────────────────────────────────┘
 *
 * Sub-components (all private, owned):
 *   VDPState   — registers, VRAM/CRAM/VSRAM, DMA state, HV counter
 *   VDPPort    — control port, data port, DMA execution
 *   VDPTile    — tile decoding, colour conversion
 *   VDPRenderer — scanline evaluation and compositing
 *   Framebuffer — pixel output buffer
 *
 * Sub-components are composed in VDP (not exposed publicly) so that the
 * game thread sees only the VDP interface.
 */
class VDP {
    public:
    /// Render synchronization mode for framerate control.
    enum Synchronization : int {
        InternalTimer = 0, ///< Use the environment's internal timer (regional rate or host turbo override)
        VSync         = 1, ///< Sync to monitor refresh rate (1:1)
        VSync2        = 2, ///< Hold frame for 2 monitor refreshes (half speed)
        VSync3        = 3, ///< Hold frame for 3 monitor refreshes (third speed)
    };

    /// Output scaling mode.
    enum Scaling : int {
        Integer = 0,  ///< Max integer scale that fits the usable display area
        Fit     = -1, ///< Fit window with bilinear filtering
        Scale1x = 1,  ///< 1x scaling (320×224)
        Scale2x = 2,  ///< 2x scaling (640×448)
        Scale3x = 3,  ///< 3x scaling (960×672)
    };

    /// Sprite rendering policy. Hardware sets SOVR either way; this controls whether excess sprites are clipped.
    enum SpriteLimitMode : int {
        HardwareSpriteLimit = 0,
        QuasiUnlimitedSprites = 1,
    };

    /// Initializes VDP emulator with given sync and scaling modes. Creates the
    /// SDL window/renderer but does not spawn the render thread — call start().
    /// @param env Owning environment; receives vSync()/hSync() callbacks and
    ///            provides system memory for DMA. Must outlive this VDP.
    VDP(MegaDriveEnvironment *env,
        Synchronization synchronization,
        Scaling scaling,
        SpriteLimitMode spriteLimitMode = HardwareSpriteLimit);

    /// Shuts down render thread and releases all resources.
    ~VDP();

    // ── Threading ────────────────────────────────────────────────────────────

    /// Spawns the render thread. Safe to call once; no-op if already running.
    void start();

    /// Stops the render thread (keeps SDL resources alive). Idempotent.
    void stop();

    // ── Port I/O ───────────────────────────────────────────────────────────

    /// Writes to VDP control port ($C00004). Handles address set and register writes.
    void writeControlPort(m_word value);

    /// Reads VDP status register from control port. Clears pending second word flag.
    m_word readControlPort();

    /// Writes to VDP data port ($C00000). Routes to VRAM/CRAM/VSRAM based on code register.
    void writeDataPort(m_word value);

    /// Reads from VDP data port. Uses read-ahead buffering per hardware spec.
    m_word readDataPort();

    /// Reads H and V counter (position within scanline). H=low byte, V=high byte.
    m_word readHVCounter();

    // ── Interrupts ───────────────────────────────────────────────────────────

    /// A display interrupt scheduled by the render thread.
    struct Interrupt {
        enum Type {
            HSync, ///< Horizontal blank: one per scanline; @c line holds the scanline index.
            VSync, ///< Vertical blank: one per frame.
        };
        Type type;
        int  line; ///< Scanline index for HSync; 0 for VSync.
    };

    /// Pops the oldest scheduled interrupt into @p out. Returns false when none
    /// are pending. Thread-safe; intended to be drained from the program thread
    /// (see MegaDriveEnvironment::runVDPInterrupts).
    bool popInterrupt(Interrupt &out);

    // ── Remote inspection / synchronization ───────────────────────────────

    struct FramebufferSnapshot {
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint16_t pitch = 0;
        std::vector<m_byte> pixels; ///< Packed BGR, one byte (0-7) per channel.
    };

    struct StateSnapshot {
        std::array<m_byte, VDPState::REG_COUNT> regs{};
        std::array<m_byte, VDPState::VRAM_SIZE> vram{};
        std::array<m_word, VDPState::CRAM_ENTRIES> cram{};
        std::array<m_word, VDPState::VSRAM_ENTRIES> vsram{};
        std::array<m_byte, VDPState::SAT_SIZE> sat{};
        m_word status = 0;
        m_word hCounter = 0;
        m_word vCounter = 0;
        std::uint16_t activeWidth = 0;
        std::uint16_t activeHeight = 0;
        std::uint16_t outputHeight = 0;
        std::uint16_t planeWidthCells = 0;
        std::uint16_t planeHeightCells = 0;
        std::uint16_t planeABase = 0;
        std::uint16_t planeBBase = 0;
        std::uint16_t windowBase = 0;
        std::uint16_t windowWidthCells = 0;
        std::uint16_t satBase = 0;
    };

    /// Coherent copies taken under the VDP state mutex.
    FramebufferSnapshot framebufferSnapshot() const;
    StateSnapshot stateSnapshot() const;

    /// Wait for events occurring strictly after the call begins. Timeouts are
    /// host milliseconds. HSync events represent every rendered active line,
    /// independently of whether the emulated HBlank interrupt is enabled.
    bool waitForVSyncCount(std::uint32_t count, std::uint32_t timeoutMs);
    bool waitForHSyncCount(std::uint32_t count, std::uint32_t timeoutMs);
    bool waitForHSyncLine(std::uint16_t line, std::uint32_t timeoutMs);
    /// Interrupts host-side waits during environment shutdown.
    void wakeSyncWaiters();

    // ── Debug ──────────────────────────────────────────────────────────────

    /// Exports current framebuffer to PNG file.
    void dumpFrameBufferToPNG(std::string path, bool fullRange);

    /// Exports framebuffer + tile sheets + plane nametables to PNG.
    void dumpEverythingToPNG(std::string path, bool fullRange);

    // ── Lifecycle ──────────────────────────────────────────────────────────

    /// Resets all VDP state to initial values and clears framebuffer.
    void reset();

    /// Stops render thread and releases SDL resources.
    void shutdown();

    private:
    /// VDP internal state (registers, memory, HV counter).
    VDPState state_;
    /// Tile decoder and color converter.
    VDPTile tile_;
    /// Control/data port I/O handlers.
    VDPPort port_;
    /// Scanline renderer (planes, sprites, window, composition).
    VDPRenderer renderer_;
    /// Debug PNG export helper.
    VDPRendererDebug rendererDebug_;
    /// Output framebuffer (up to 320×480, 3 bits per channel).
    Framebuffer framebuffer_;

    /// SDL window handle for display.
    SDL_Window *window_ = nullptr;
    /// SDL renderer for texture presentation.
    SDL_Renderer *sdlRenderer_ = nullptr;
    /// SDL texture for framebuffer display.
    SDL_Texture *texture_ = nullptr;
    /// Render thread handle.
    SDL_Thread *thread_ = nullptr;
    /// Mutex protecting state_ from concurrent access.
    SDL_Mutex *mutex_ = nullptr;
    /// Flag controlling render thread loop.
    std::atomic<bool> running_{false};

    /// Event timeline used by remote waits; deliberately separate from the
    /// emulated interrupt queue because automation observes physical syncs.
    mutable std::mutex syncEventMutex_;
    std::condition_variable syncEventCV_;
    std::uint64_t vSyncGeneration_ = 0;
    std::uint64_t hSyncGeneration_ = 0;
    std::uint64_t syncWakeGeneration_ = 0;
    std::array<std::uint64_t, VDPState::MAX_SCREEN_H> hSyncLineGenerations_{};

    void signalHSync(int line);
    void signalVSync();

    /// Rolling average of the latest 60 complete frame intervals, published
    /// by the render thread and consumed by the SDL main thread.
    std::atomic<uint64_t> averageFrameTimeNs_{0};
    /// Last average shown in the title; only accessed on the SDL main thread.
    uint64_t displayedFrameTimeNs_ = 0;
    /// Debug cadence uses host time so turbo does not multiply log volume.
    unsigned debugFrame_ = 0;
    std::uint64_t nextDebugLogNs_ = 0;

    /// Selected synchronization mode.
    Synchronization syncMode_;
    /// Selected scaling mode.
    Scaling scalingMode_;

    /// Owning environment; provides system memory for DMA (via VDPPort).
    MegaDriveEnvironment *env_ = nullptr;

    // ── Interrupt scheduling ───────────────────────────────────────────────

    /// Queue of interrupts scheduled by the render thread, drained by popInterrupt().
    std::deque<Interrupt> irqQueue_;
    /// Protects irqQueue_.
    SDL_Mutex *irqMutex_ = nullptr;
    /// Upper bound on pending interrupts; oldest are dropped past this so a
    /// program that never services interrupts cannot grow the queue unbounded.
    static constexpr size_t IRQ_QUEUE_MAX = VDPState::MAX_SCREEN_H * 3;

    /// Appends an interrupt to irqQueue_ (drops the oldest if at capacity).
    void scheduleInterrupt(Interrupt::Type type, int line);

    // ── Thread ─────────────────────────────────────────────────────────────

    /// Static entry point for render thread. Delegates to renderLoop().
    static int renderThreadEntry(void *data);

    /// Main render loop: renders frame, signals VBlank, presents to display, manages frame timing.
    int renderLoop();

    /// SDL main-thread callback to present texture to screen.
    static void sdlPresentCallback(void *userdata);

    /// SDL main-thread callback to repeat texture display (for VSync2/VSync3).
    static void sdlRepeatCallback(void *userdata);

    /// Converts framebuffer to SDL texture and renders to window.
    void presentToScreen();

    /// Updates the SDL window title with the rolling average frame time.
    void updateWindowTitle();
};
