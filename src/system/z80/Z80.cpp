#include "Z80.hpp"

#include "system/MegaDriveEnvironment.hpp"
#include "Logger.hpp"

// Keep the vendor core lean: no debug/breakpoint/call-nest machinery.
#define Z80_DISABLE_DEBUG
#define Z80_DISABLE_BREAKPOINT
#define Z80_DISABLE_NESTCHECK
#include "system/z80/suzukiplan/z80.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
// Mega Drive Z80 T-states map to master cycles 1:15 (7.6 MHz vs 53.7 MHz).
constexpr uint32_t kMasterCyclesPerZ80TState = 15;
constexpr uint32_t kMaxRunChunkTStates       = 512;
// Genesis Plus GX asserts the Z80 INT at VBlank and clears it at the end of
// that scanline; hold it for one scanline of emulated time (~228 T-states).
constexpr uint64_t kIRQHoldTStates = 228;
// After a stall (bus held, host hiccup), don't catch up more than ~2 frames
// of Z80 time: real hardware doesn't catch up either, and a large burst would
// stamp sound events far ahead of the shared wall clock.
constexpr uint64_t kMaxBacklogMasterCycles = 120'000ull * 15ull;
// How much catch-up a bus release may run synchronously on the caller's
// thread. The 68K sound driver toggles BUSREQ at native speed while waiting
// for the Z80's DAC flag; the release window must actually execute Z80 code
// (as it does for a few microseconds on hardware) or that handshake livelocks.
constexpr uint32_t kBusReleaseTStates = 2048;
// Per-pass budget for the Z80 thread.
constexpr uint32_t kThreadRunTStates = 32'768;
// Mega Drive VDP asserts /INT with data bus typically read as $FF in IM 1;
// mode 1 ignores the vector and vectors to $0038, but mode 0 would use it.
constexpr unsigned char kIRQVector = 0xFF;
} // namespace

struct Z80::Core {
    suzukiplan::Z80 cpu;

    explicit Core(Z80 *owner)
        : cpu(&Z80::staticRead8,
              &Z80::staticWrite8,
              &Z80::staticReadPort,
              &Z80::staticWritePort,
              owner) {}
};

// ymfm/PSG writes are stamped with the core timeline. The previous GPX core
// exposed a live cycle counter during memory callbacks; suzukiplan only returns
// total clocks after execute() unless we mirror progress via consumeClock.
void Z80::installClockCallback() {
    core_->cpu.setConsumeClockCallback([](void *arg, int clocks) {
        if (clocks <= 0)
            return;
        auto *self = static_cast<Z80 *>(arg);
        self->executedCoreMasterCycles_ +=
            static_cast<uint64_t>(clocks) * kMasterCyclesPerZ80TState;
    });
}

Z80::Z80(MegaDriveEnvironment *env) : env_(env), mutex_(SDL_CreateMutex()), core_(std::make_unique<Core>(this)) {
    fallbackBaseNS_ = SDL_GetTicksNS();
    SDL_LockMutex(mutex_);
    resetCPU();
    SDL_UnlockMutex(mutex_);
}

uint64_t Z80::wallMasterCycles() const {
    if (env_)
        return env_->sound().masterCyclesNow();
    // env-less (tests): local wall clock so the core still paces itself.
    const uint64_t ns = SDL_GetTicksNS() - fallbackBaseNS_;
    return (ns / 1'000'000'000ull) * 53'693'175ull + ((ns % 1'000'000'000ull) * 53'693'175ull) / 1'000'000'000ull;
}

Z80::~Z80() {
    stop();
    if (mutex_) {
        SDL_DestroyMutex(mutex_);
        mutex_ = nullptr;
    }
}

void Z80::start() {
    if (running_.exchange(true))
        return;
    thread_ = SDL_CreateThread(threadEntry, "Z80", this);
}

void Z80::stop() {
    running_.store(false);
    if (thread_) {
        SDL_WaitThread(thread_, nullptr);
        thread_ = nullptr;
    }
}

void Z80::reset() {
    stop();
    SDL_LockMutex(mutex_);
    ram_.fill(0);
    resetCPU();
    busRequested_.store(true, std::memory_order_release);
    busAcked_.store(true, std::memory_order_release);
    resetHeld_.store(true, std::memory_order_release);
    irqPending_.store(false, std::memory_order_release);
    fallbackBaseNS_ = SDL_GetTicksNS();
    SDL_UnlockMutex(mutex_);
}

void Z80::setBusRequest(bool requested) {
    busRequested_.store(requested, std::memory_order_release);
    if (requested) {
        // The ack must come from the Z80 thread once it actually stops between
        // run chunks: acking here would let the 68K touch the YM2612 while the
        // core is still executing (SOR's 68K driver serializes its FM writes
        // against the Z80's DAC writes through busreq + a Z80-RAM flag).
        if (resetHeld_.load(std::memory_order_acquire) || !running_.load(std::memory_order_acquire))
            busAcked_.store(true, std::memory_order_release);
    } else if (!resetHeld_.load(std::memory_order_acquire)) {
        busAcked_.store(false, std::memory_order_release);
        // Run the catch-up synchronously so the release window executes Z80
        // code even when the caller re-requests the bus nanoseconds later
        // (native-speed 68K). Bounded by the real wall-clock deficit, so it
        // cannot drift the timeline ahead.
        SDL_LockMutex(mutex_);
        runTowardWallClock(kBusReleaseTStates);
        SDL_UnlockMutex(mutex_);
    }
}

bool Z80::busRequestAcked() const {
    return busAcked_.load(std::memory_order_acquire);
}

void Z80::setReset(bool held) {
    const bool wasHeld = resetHeld_.exchange(held, std::memory_order_acq_rel);
    if (held) {
        busAcked_.store(true, std::memory_order_release);
        return;
    }

    if (wasHeld) {
        SDL_LockMutex(mutex_);
        resetCPU();
        SDL_UnlockMutex(mutex_);
    }

    if (!busRequested_.load(std::memory_order_acquire))
        busAcked_.store(false, std::memory_order_release);
}

void Z80::pulseVBlankIRQ() {
    irqPending_.store(true, std::memory_order_release);
}

int Z80::threadEntry(void *data) {
    static_cast<Z80 *>(data)->runThread();
    return 0;
}

void Z80::runThread() {
    const bool traceRate    = std::getenv("SOR_Z80_TRACE") != nullptr;
    uint64_t   traceLastNS  = SDL_GetTicksNS();
    uint64_t   traceStalls  = 0;
    uint64_t   traceCycles0 = 0;

    while (running_.load(std::memory_order_acquire)) {
        if (traceRate) {
            const uint64_t now = SDL_GetTicksNS();
            if (now - traceLastNS >= 1'000'000'000ull) {
                const uint64_t cycles = cycleEpochMasterCycles_ + executedCoreMasterCycles_;
                Logger::log("[z80] tstates/s=%llu stallPolls/s=%llu",
                            static_cast<unsigned long long>((cycles - traceCycles0) / kMasterCyclesPerZ80TState),
                            static_cast<unsigned long long>(traceStalls));
                traceCycles0 = cycles;
                traceStalls  = 0;
                traceLastNS  = now;
            }
        }

        if (resetHeld_.load(std::memory_order_acquire) || busRequested_.load(std::memory_order_acquire)) {
            busAcked_.store(true, std::memory_order_release);
            ++traceStalls;
            SDL_DelayNS(50'000);
            continue;
        }

        busAcked_.store(false, std::memory_order_release);

        SDL_LockMutex(mutex_);
        runTowardWallClock(kThreadRunTStates);
        SDL_UnlockMutex(mutex_);

        SDL_DelayNS(100'000);
    }
}

// Chases the shared wall clock: runs the core (in chunks) until its timeline
// catches up with "now", executing at most maxTStates in this call. Backlog
// beyond kMaxBacklogMasterCycles is dropped, like the real chip, which never
// catches up on time it spent bus-stalled. The single source of pacing truth
// is the wall clock, so the timeline can never drift ahead of it.
void Z80::runTowardWallClock(uint32_t maxTStates) {
    const uint64_t now      = wallMasterCycles();
    uint64_t       position = cycleEpochMasterCycles_ + executedCoreMasterCycles_;
    if (position >= now)
        return;

    uint64_t deficit = now - position;
    if (deficit > kMaxBacklogMasterCycles) {
        cycleEpochMasterCycles_ += deficit - kMaxBacklogMasterCycles;
        deficit = kMaxBacklogMasterCycles;
    }

    uint64_t budget = std::min<uint64_t>(deficit / kMasterCyclesPerZ80TState, maxTStates);
    while (budget >= 4) {
        if (resetHeld_.load(std::memory_order_acquire) || busRequested_.load(std::memory_order_acquire))
            break;
        const uint32_t tStates = static_cast<uint32_t>(std::min<uint64_t>(budget, kMaxRunChunkTStates));
        runCoreForTStates(tStates);
        budget -= tStates;
    }
}

void Z80::resetCPU() {
    core_->cpu.initialize();
    // initialize() clears the consumeClock hook; restore the live timeline.
    installClockCallback();
    // Mega Drive sound drivers almost always use interrupt mode 1 (RST $38).
    core_->cpu.reg.interrupt = 0b00000001;
    executedCoreMasterCycles_ = 0;
    cycleEpochMasterCycles_   = 0;
    bankRegister_             = 0;
    irqClearAtMasterCycles_   = 0;
    irqLineAsserted_          = false;
    irqPending_.store(false, std::memory_order_release);
}

void Z80::runCoreForTStates(uint32_t tStates) {
    if (tStates == 0)
        return;

    if (irqPending_.exchange(false, std::memory_order_acq_rel)) {
        core_->cpu.generateIRQ(kIRQVector);
        irqLineAsserted_        = true;
        irqClearAtMasterCycles_ = executedCoreMasterCycles_ + (kIRQHoldTStates * kMasterCyclesPerZ80TState);
    }

    // execute() advances executedCoreMasterCycles_ via installClockCallback()
    // so YM/PSG port writes mid-batch get distinct timestamps.
    auto runSlice = [this](uint32_t sliceTStates) {
        if (sliceTStates == 0)
            return;
        (void)core_->cpu.execute(static_cast<int>(sliceTStates));
    };

    const uint64_t targetMasterCycles =
        executedCoreMasterCycles_ + (static_cast<uint64_t>(tStates) * kMasterCyclesPerZ80TState);

    if (irqLineAsserted_ && irqClearAtMasterCycles_ < targetMasterCycles) {
        if (irqClearAtMasterCycles_ > executedCoreMasterCycles_) {
            const uint64_t remainingMaster = irqClearAtMasterCycles_ - executedCoreMasterCycles_;
            const uint32_t untilClear =
                static_cast<uint32_t>((remainingMaster + kMasterCyclesPerZ80TState - 1) / kMasterCyclesPerZ80TState);
            runSlice(untilClear);
        }
        core_->cpu.cancelIRQ();
        irqLineAsserted_ = false;
    }

    if (executedCoreMasterCycles_ < targetMasterCycles) {
        const uint64_t remainingMaster = targetMasterCycles - executedCoreMasterCycles_;
        const uint32_t remainingTStates =
            static_cast<uint32_t>((remainingMaster + kMasterCyclesPerZ80TState - 1) / kMasterCyclesPerZ80TState);
        runSlice(remainingTStates);
    }
}

uint64_t Z80::currentMasterCyclesForCore() const {
    return cycleEpochMasterCycles_ + executedCoreMasterCycles_;
}

m_byte Z80::readRAMFor68K(uint16_t address) {
    SDL_LockMutex(mutex_);
    const m_byte value = ram_[address & 0x1FFFu];
    SDL_UnlockMutex(mutex_);
    return value;
}

void Z80::writeRAMFor68K(uint16_t address, m_byte value) {
    SDL_LockMutex(mutex_);
    ram_[address & 0x1FFFu] = value;
    SDL_UnlockMutex(mutex_);
}

uint8_t Z80::read8ForCore(uint16_t address) {
    if (address < 0x4000)
        return ram_[address & 0x1FFFu];

    if (address >= 0x4000 && address < 0x6000)
        return env_->sound().readYM2612At(currentMasterCyclesForCore(), address & 3u);

    if ((address & 0xFF00u) == 0x7F00u)
        return env_->memory().readByte(0x00C00000u | (address & 0x00FFu));

    if (address >= 0x8000) {
        const uint32_t m68kAddress = ((bankRegister_ << 15) + (address & 0x7FFFu)) & 0x00FFFFFFu;
        return env_->memory().readByte(m68kAddress);
    }

    return 0xFF;
}

void Z80::write8ForCore(uint16_t address, uint8_t value) {
    if (address < 0x4000) {
        ram_[address & 0x1FFFu] = value;
        return;
    }

    if (address >= 0x4000 && address < 0x6000) {
        env_->sound().writeYM2612At(currentMasterCyclesForCore(), address & 3u, value);
        return;
    }

    if ((address & 0xFF00u) == 0x6000u) {
        bankRegister_ = ((bankRegister_ >> 1) | ((value & 1u) << 8)) & 0x1FFu;
        return;
    }

    if ((address & 0xFF00u) == 0x7F00u) {
        const uint32_t vdpAddress = 0x00C00000u | (address & 0x00FFu);
        if (vdpAddress >= 0x00C00010u && vdpAddress < 0x00C00018u && (vdpAddress & 1u) != 0)
            env_->sound().writePSGAt(currentMasterCyclesForCore(), value);
        else
            env_->memory().writeByte(vdpAddress, value);
        return;
    }

    if (address >= 0x8000) {
        const uint32_t m68kAddress = ((bankRegister_ << 15) + (address & 0x7FFFu)) & 0x00FFFFFFu;
        env_->memory().writeByte(m68kAddress, value);
    }
}

uint8_t Z80::readPortForCore(uint16_t port) {
    (void)port;
    return 0xFF;
}

void Z80::writePortForCore(uint16_t port, uint8_t value) {
    (void)port;
    (void)value;
}

uint8_t Z80::staticRead8(void *arg, unsigned short address) {
    return static_cast<Z80 *>(arg)->read8ForCore(static_cast<uint16_t>(address));
}

void Z80::staticWrite8(void *arg, unsigned short address, unsigned char value) {
    static_cast<Z80 *>(arg)->write8ForCore(static_cast<uint16_t>(address), value);
}

uint8_t Z80::staticReadPort(void *arg, unsigned short port) {
    return static_cast<Z80 *>(arg)->readPortForCore(static_cast<uint16_t>(port));
}

void Z80::staticWritePort(void *arg, unsigned short port, unsigned char value) {
    static_cast<Z80 *>(arg)->writePortForCore(static_cast<uint16_t>(port), value);
}
