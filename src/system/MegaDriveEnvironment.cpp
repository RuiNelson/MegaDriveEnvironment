/**
 * @file MegaDriveEnvironment.cpp
 * @brief Root environment implementation.
 */

#include "MegaDriveEnvironment.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include "Logger.hpp"

namespace {

bool auxFileContainsAddress(const std::string &path, unsigned addr) {
    if (FILE *in = std::fopen(path.c_str(), "r")) {
        char line[64];
        while (std::fgets(line, sizeof line, in)) {
            char *p = line;
            while (*p == ' ' || *p == '\t')
                ++p;
            if (*p == ';' || *p == '#' || *p == '\r' || *p == '\n' || *p == '\0')
                continue;
            if ((static_cast<unsigned>(std::strtoul(p, nullptr, 16)) & 0x00FFFFFFu) == addr) {
                std::fclose(in);
                return true;
            }
        }
        std::fclose(in);
    }
    return false;
}

} // namespace

MegaDriveEnvironment::MegaDriveEnvironment(VDP::Synchronization sync,
                                           VDP::Scaling scaling,
                                           VDP::SpriteLimitMode spriteLimitMode,
                                           std::uint16_t remoteAccessPort)
    : memory_(this), z80_(this), sound_(this), controllers_(this), vdp_(this, sync, scaling, spriteLimitMode),
      remoteAccess_(this, remoteAccessPort) {
}

MegaDriveEnvironment::~MegaDriveEnvironment() {
    // Safety net in case boot() did not run to completion.
    if (cpuThread_) {
        SDL_WaitThread(cpuThread_, nullptr);
        cpuThread_ = nullptr;
    }
    closeOptionHotkeyGamepads();
    remoteAccess_.stop();
    powerOff();
}

void MegaDriveEnvironment::boot() {
    quitRequested_.store(false, std::memory_order_release);
    restartRequested_.store(false, std::memory_order_release);
    bootRunning_.store(true, std::memory_order_release);
    remoteAccess_.start();
    powerOn(false);
    openOptionHotkeyGamepads();

    // Pump SDL events on the main thread so the VDP can present frames
    // (SDL_RunOnMainThread) and so window-close requests are observed.
    SDL_Event event;
    bool      fullscreenActive = false;
    bool      cursorWasVisible = true;
    bool      runAgain = true;
    while (runAgain) {
        cpuDone_.store(false, std::memory_order_release);
        cpuThread_ = SDL_CreateThread(cpuThreadEntry, "CPU", this);
        if (!cpuThread_) {
            Logger::log("[MDE] could not create CPU thread: %s", SDL_GetError());
            break;
        }

        while (!cpuDone_.load(std::memory_order_acquire)) {
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
                    openOptionHotkeyGamepad(event.gdevice.which);
                } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
                    closeOptionHotkeyGamepad(event.gdevice.which);
                } else if (event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN && !fullscreenActive) {
                    cursorWasVisible = SDL_CursorVisible();
                    SDL_HideCursor();
                    fullscreenActive = true;
                } else if (event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN && fullscreenActive) {
                    if (cursorWasVisible) {
                        SDL_ShowCursor();
                    }
                    fullscreenActive = false;
                }

                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && (event.key.mod & SDL_KMOD_ALT) &&
                    event.key.key != SDLK_LALT && event.key.key != SDLK_RALT) {
                    handleOptionHotkey(OptionHotkeyCode{
                        .source      = OptionHotkeyCode::Source::Keyboard,
                        .keyboardKey = event.key.key,
                    });
                } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && (SDL_GetModState() & SDL_KMOD_ALT)) {
                    handleOptionHotkey(OptionHotkeyCode{
                        .source        = OptionHotkeyCode::Source::Gamepad,
                        .gamepadButton = static_cast<SDL_GamepadButton>(event.gbutton.button),
                        .gamepadId     = event.gbutton.which,
                    });
                }

                // Window close or CTRL+Q both request a shutdown. CTRL+R performs
                // a cold restart without replacing the process or TCP connection.
                if (event.type == SDL_EVENT_QUIT) {
                    quitRequested_.store(true, std::memory_order_release);
                    interruptGeneration_.fetch_add(1, std::memory_order_release);
                    interruptGeneration_.notify_all();
                } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_Q &&
                           (event.key.mod & SDL_KMOD_CTRL)) {
                    quitRequested_.store(true, std::memory_order_release);
                    interruptGeneration_.fetch_add(1, std::memory_order_release);
                    interruptGeneration_.notify_all();
                } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_R &&
                           (event.key.mod & SDL_KMOD_CTRL)) {
                    requestRestart();
                } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_P &&
                           (event.key.mod & SDL_KMOD_CTRL)) {
                    // CTRL+P: capture the composited frame to a numbered PNG.
                    static unsigned shot = 0;
                    char            path[64];
                    std::snprintf(path, sizeof path, "screenshot_%03u.png", shot++);
                    vdp_.dumpFrameBufferToPNG(path, /*fullRange=*/true);
                    Logger::log("[capture] frame -> %s", path);
                } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_S &&
                           (event.key.mod & SDL_KMOD_CTRL)) {
                    // CTRL+S: capture the full VDP debug view (frame + tile sheets +
                    // plane nametables + registers) to a numbered PNG.
                    static unsigned shot = 0;
                    char            path[64];
                    std::snprintf(path, sizeof path, "vpd_%03u.png", shot++);
                    vdp_.dumpEverythingToPNG(path, /*fullRange=*/true);
                    Logger::log("[capture] VDP debug view -> %s", path);
                }
            }
            if (quitRequested_.load(std::memory_order_acquire)) {
                // Give a cooperative run() — one that polls shouldQuit() — a brief
                // window to leave its loop and return for an orderly shutdown, while
                // still pumping events so the VDP can present.
                for (int i = 0; i < 100 && !cpuDone_.load(std::memory_order_acquire); ++i) {
                    while (SDL_PollEvent(&event)) {
                    }
                    SDL_DelayNS(500'000);
                }
                if (!cpuDone_.load(std::memory_order_acquire)) {
                    // run() did not return — e.g. the mechanically-generated SoR main
                    // loop is an unbounded `bra` that never polls shouldQuit(). Force
                    // the process down: returning from main() does not reliably end it
                    // on macOS, where SDL parks the real main thread in its own run
                    // loop to service SDL_RunOnMainThread (the VDP's frame
                    // presentation). _Exit() tears every thread down immediately and
                    // lets the OS reclaim the window.
                    std::_Exit(0);
                }
                break;
            }
            SDL_DelayNS(250'000);
        }

        SDL_WaitThread(cpuThread_, nullptr);
        cpuThread_ = nullptr;

        runAgain = false;
        if (!quitRequested_.load(std::memory_order_acquire)) {
            std::lock_guard lock(restartMutex_);
            if (restartRequested_.exchange(false, std::memory_order_acq_rel)) {
                restartingGeneration_ = restartRequestedGeneration_;
                runAgain = true;
            }
        }
        if (runAgain) {
            powerOff();
            powerOn(true);
        }
    }

    if (fullscreenActive && cursorWasVisible) {
        SDL_ShowCursor();
    }
    closeOptionHotkeyGamepads();
    powerOff();
    bootRunning_.store(false, std::memory_order_release);
    restartCondition_.notify_all();
    remoteAccess_.stop();
}

std::uint64_t MegaDriveEnvironment::requestRestart() {
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(restartMutex_);
        if (!bootRunning_.load(std::memory_order_acquire) || quitRequested_.load(std::memory_order_acquire))
            return 0;
        if (!restartRequested_.load(std::memory_order_acquire)) {
            restartRequested_.store(true, std::memory_order_release);
            ++restartRequestedGeneration_;
        }
        generation = restartRequestedGeneration_;
    }
    interruptGeneration_.fetch_add(1, std::memory_order_release);
    interruptGeneration_.notify_all();
    return generation;
}

bool MegaDriveEnvironment::restart(std::uint32_t timeoutMs) {
    if (timeoutMs == 0)
        return false;
    const std::uint64_t generation = requestRestart();
    if (generation == 0)
        return false;
    std::unique_lock lock(restartMutex_);
    return restartCondition_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        return restartStartedGeneration_ >= generation || !bootRunning_.load(std::memory_order_acquire);
    }) && restartStartedGeneration_ >= generation;
}

std::uint64_t MegaDriveEnvironment::gameUptimeMilliseconds() const {
    std::chrono::steady_clock::time_point startedAt;
    {
        std::lock_guard lock(restartMutex_);
        startedAt = gameStartedAt_;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);
    return elapsed.count() > 0 ? static_cast<std::uint64_t>(elapsed.count()) : 0;
}

void MegaDriveEnvironment::reportCPUStarted() {
    std::lock_guard lock(restartMutex_);
    if (restartingGeneration_ != 0) {
        restartStartedGeneration_ = std::max(restartStartedGeneration_, restartingGeneration_);
        restartingGeneration_ = 0;
        restartCondition_.notify_all();
    }
}

void MegaDriveEnvironment::openOptionHotkeyGamepads() {
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (!ids)
        return;
    for (int i = 0; i < count; ++i)
        openOptionHotkeyGamepad(ids[i]);
    SDL_free(ids);
}

void MegaDriveEnvironment::openOptionHotkeyGamepad(SDL_JoystickID id) {
    if (id == 0 || SDL_GetGamepadFromID(id) || optionHotkeyGamepads_.contains(id))
        return;
    if (SDL_Gamepad *gamepad = SDL_OpenGamepad(id))
        optionHotkeyGamepads_.emplace(id, gamepad);
}

void MegaDriveEnvironment::closeOptionHotkeyGamepad(SDL_JoystickID id) {
    const auto it = optionHotkeyGamepads_.find(id);
    if (it == optionHotkeyGamepads_.end())
        return;
    SDL_CloseGamepad(it->second);
    optionHotkeyGamepads_.erase(it);
}

void MegaDriveEnvironment::closeOptionHotkeyGamepads() {
    for (const auto &[id, gamepad] : optionHotkeyGamepads_) {
        (void)id;
        SDL_CloseGamepad(gamepad);
    }
    optionHotkeyGamepads_.clear();
}

m_byte MegaDriveEnvironment::hardwareVersionRegister() const {
    m_byte value = 0x00; // Model 1, no TMSS, JP + 60 Hz by default.
    if (languagePin() == LanguagePin::Overseas)
        value |= 0x80;
    if (videoStandard() == VideoStandard::Hz50)
        value |= 0x40;
    return value;
}

void MegaDriveEnvironment::logFrame(unsigned frame, bool displayEnabled) {
    // Progression-state probe: which game mode the state machine is in ($FF00),
    // the VBlank-routine / palette-upload requests ($FA00/$FA01), the intro
    // counter the title build polls ($FA30), the VBlank jump-table index ($FF06),
    // and two samples of the master palette buffer ($FFF400) so a stuck fade
    // shows up as palette words that never brighten frame to frame.
    m_word mode = memory_.readWord(0xFFFFFF00u);
    m_byte fa30 = memory_.readByte(0xFFFFFA30u);
    m_word fb06 = memory_.readWord(0xFFFFFB06u); // story-step frame delay

    // Palette pipeline: city palette -> $DD80 (master) -> fade by $FA61 -> $F400
    // (live CRAM buffer) -> DMA to CRAM. Sample the master and the live buffer at
    // the same offset, plus the fade level, to see where the chain breaks.
    m_word mst08 = memory_.readWord(0xFFFFDD90u); // master palette, entry 8
    m_word mst18 = memory_.readWord(0xFFFFDDA0u); // master palette, entry 16
    m_word liv08 = memory_.readWord(0xFFFFF410u); // live buffer, entry 8
    m_byte fa61  = memory_.readByte(0xFFFFFA61u); // fade level
    m_byte fa63  = memory_.readByte(0xFFFFFA63u); // fade flag
    m_byte fa05  = memory_.readByte(0xFFFFFA05u); // fade mode flags

    Logger::log("[MDE] t=%2us frame=%u  IPL=%d  fn=$%06X  mode($FF00)=%04X cnt($FA30)=%02X gate($FB06)=%04X  "
                "master[$DD90]=%04X master[$DDA0]=%04X  live[$F410]=%04X  fade($FA61)=%02X f63=%02X f05=%02X",
                frame / 60,
                frame,
                cpuInterruptMask(),
                traceFn_.load(std::memory_order_relaxed),
                mode,
                fa30,
                fb06,
                mst08,
                mst18,
                liv08,
                fa61,
                fa63,
                fa05);

    const Sound::Diagnostics snd = sound_.diagnostics();
    Logger::log("[snd] frames=%llu queued=%llu late=%llu drop=%llu(cont=%llu full=%llu unavailable=%llu) "
                "under=%llu over=%llu "
                "clip=%llu timers=%llu peakL=%d "
                "peakR=%d",
                static_cast<unsigned long long>(snd.audioFramesRendered),
                static_cast<unsigned long long>(snd.queuedEvents),
                static_cast<unsigned long long>(snd.lateEvents),
                static_cast<unsigned long long>(snd.droppedEvents),
                static_cast<unsigned long long>(snd.contentionDrops),
                static_cast<unsigned long long>(snd.queueFullDrops),
                static_cast<unsigned long long>(snd.unavailableDrops),
                static_cast<unsigned long long>(snd.underruns),
                static_cast<unsigned long long>(snd.overruns),
                static_cast<unsigned long long>(snd.clippedSamples),
                static_cast<unsigned long long>(snd.ymTimerExpirations),
                snd.peakLeft,
                snd.peakRight);
}

void MegaDriveEnvironment::confirmSpeculative(m_long addr) {
    if (auxAddrFile_.empty())
        return;

    unsigned a = static_cast<unsigned>(addr & 0x00FFFFFFu);
    if (!confirmedSpeculative_.insert(a).second)
        return; // already logged this run
    if (auxFileContainsAddress(auxAddrFile_, a))
        return; // already known; not a newly confirmed speculative candidate
    std::fprintf(stderr, "[speculative] confirmed: %06X\n", a);
    if (FILE *out = std::fopen(auxAddrFile_.c_str(), "a")) {
        std::fprintf(out, "%06X\n", a);
        std::fclose(out);
    }
}

void MegaDriveEnvironment::reportUnhandledDispatch(m_long addr) {
    unsigned a  = static_cast<unsigned>(addr & 0x00FFFFFFu);
    unsigned fn = static_cast<unsigned>(lastFunction() & 0x00FFFFFFu);

    std::fprintf(stderr, "indirect dispatch to unknown address $%06X (in fn $%06X)\n", a, fn);

    dumpUnhandledDispatchCpuState();

    std::fprintf(stderr,
                 "[dispatch] RAM FF00=%04X FB15=%02X FB06=%04X FA1A=%02X "
                 "FA30=%02X F904=%02X F905=%02X\n",
                 static_cast<unsigned>(memory_.readWord(0xFFFFFF00u)),
                 static_cast<unsigned>(memory_.readByte(0xFFFFFB15u)),
                 static_cast<unsigned>(memory_.readWord(0xFFFFFB06u)),
                 static_cast<unsigned>(memory_.readByte(0xFFFFFA1Au)),
                 static_cast<unsigned>(memory_.readByte(0xFFFFFA30u)),
                 static_cast<unsigned>(memory_.readByte(0xFFFFF904u)),
                 static_cast<unsigned>(memory_.readByte(0xFFFFF905u)));
    std::fprintf(stderr, "[dispatch] trace history:");
    unsigned firstTrace = traceHistoryPos_ > 16 ? traceHistoryPos_ - 16 : 0;
    for (unsigned i = firstTrace; i < traceHistoryPos_; ++i) {
        std::fprintf(stderr, " $%06X", static_cast<unsigned>(traceHistory_[i & 0x0Fu] & 0x00FFFFFFu));
    }
    std::fprintf(stderr, "\n");

    const bool invalidCodeTarget = (a < 0x000200u) || ((a & 1u) != 0);
    if (invalidCodeTarget) {
        std::fprintf(stderr,
                     "[aux] refusing to seed invalid code address $%06X "
                     "(68K code starts at $000200 and must be word-aligned)\n",
                     a);
        if (!auxAddrFile_.empty()) {
            std::_Exit(44);
        }
    }

    if (auxAddrFile_.empty()) {
        std::abort(); // default: no aux file configured
    }

    // Already seeded? Then a previous pass added it but regenerating produced no
    // handler — stop the discovery loop (exit 43) instead of spinning forever.
    if (auxFileContainsAddress(auxAddrFile_, a)) {
        std::fprintf(
            stderr, "[aux] $%06X already seeded in %s — seeding did not help; stopping\n", a, auxAddrFile_.c_str());
        std::_Exit(43);
    }

    // Record the new target and exit (42) so the discovery loop re-seeds and
    // regenerates. _Exit avoids the unreliable SDL/global teardown on this path.
    if (FILE *out = std::fopen(auxAddrFile_.c_str(), "a")) {
        std::fprintf(out, "%06X\n", a);
        std::fclose(out);
        std::fprintf(stderr, "[aux] recorded $%06X to %s\n", a, auxAddrFile_.c_str());
        std::_Exit(42);
    }
    std::fprintf(stderr, "[aux] could not write %s\n", auxAddrFile_.c_str());
    std::abort();
}

int MegaDriveEnvironment::cpuThreadEntry(void *data) {
    auto *self = static_cast<MegaDriveEnvironment *>(data);
    self->reportCPUStarted();
    try {
        self->run();
    } catch (const RestartSignal &) {
        // Cooperative restart: boot() joins this thread before resetting any subsystem.
    }
    self->cpuDone_.store(true, std::memory_order_release);
    return 0;
}

void MegaDriveEnvironment::runVDPInterrupts() {
    VDP::Interrupt irq;
    while (vdp_.popInterrupt(irq)) {
        switch (irq.type) {
            case VDP::Interrupt::HSync:
                hSync(irq.line);
                break;
            case VDP::Interrupt::VSync:
                vSync();
                break;
        }
    }
}

void MegaDriveEnvironment::powerOn(bool isRestart) {
    {
        std::lock_guard lock(restartMutex_);
        gameStartedAt_ = std::chrono::steady_clock::now();
    }
    gameUptimeFrames_.store(0, std::memory_order_release);
    if (isRestart)
        onReset();
    memory_.resetWorkRAM();
    controllers_.reset();
    vdp_.reset();
    z80_.reset();
    m68kMasterCycles_.store(0, std::memory_order_release);
    pendingIRQMask_.store(0, std::memory_order_release);
    traceFn_.store(0, std::memory_order_release);
    traceHistoryPos_ = 0;
    std::fill(std::begin(traceHistory_), std::end(traceHistory_), 0);
    onPowerOn();
    vdp_.start();
    sound_.start();
    z80_.start();
}

void MegaDriveEnvironment::powerOff() {
    controllers_.clearRemoteState();
    z80_.stop();
    sound_.stop();
    vdp_.stop();
}
