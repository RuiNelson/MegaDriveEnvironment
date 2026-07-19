#pragma once

#include "data_types.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

class MegaDriveEnvironment;

class Z80 {
    public:
    explicit Z80(MegaDriveEnvironment *env);
    ~Z80();

    Z80(const Z80 &)            = delete;
    Z80 &operator=(const Z80 &) = delete;

    void start();
    void stop();
    /// Restores RAM, CPU and bus/reset lines. Call while the Z80 is stopped.
    void reset();

    m_byte *ram() {
        return ram_.data();
    }

    m_byte readRAMFor68K(uint16_t address);
    void   writeRAMFor68K(uint16_t address, m_byte value);

    void setBusRequest(bool requested);
    bool busRequestAcked() const;
    void setReset(bool held);
    void pulseVBlankIRQ();

    private:
    struct Core;

    static int threadEntry(void *data);
    void       runThread();
    void       resetCPU();
    void       installClockCallback();
    void       runCoreForTStates(uint32_t tStates);
    void       runTowardWallClock(uint32_t maxTStates);
    uint64_t   wallMasterCycles() const;
    uint64_t   currentMasterCyclesForCore() const;

    uint8_t read8ForCore(uint16_t address);
    void    write8ForCore(uint16_t address, uint8_t value);
    uint8_t readPortForCore(uint16_t port);
    void    writePortForCore(uint16_t port, uint8_t value);

    static uint8_t staticRead8(void *arg, unsigned short address);
    static void    staticWrite8(void *arg, unsigned short address, unsigned char value);
    static uint8_t staticReadPort(void *arg, unsigned short port);
    static void    staticWritePort(void *arg, unsigned short port, unsigned char value);

    MegaDriveEnvironment      *env_    = nullptr;
    SDL_Thread                *thread_ = nullptr;
    SDL_Mutex                 *mutex_  = nullptr;
    std::unique_ptr<Core>      core_;
    std::array<m_byte, 0x2000> ram_{};
    std::atomic<bool>          running_{false};
    std::atomic<bool>          busRequested_{true};
    std::atomic<bool>          busAcked_{true};
    std::atomic<bool>          resetHeld_{true};
    std::atomic<bool>          irqPending_{false};
    uint32_t                   bankRegister_             = 0;
    uint64_t                   executedCoreMasterCycles_ = 0; ///< core cycle counter (master cycles, 15/T-state)
    uint64_t                   irqClearAtMasterCycles_   = 0;
    bool                       irqLineAsserted_          = false;
    uint64_t                   cycleEpochMasterCycles_   = 0;
    uint64_t                   fallbackBaseNS_           = 0; ///< wall-clock base when env_ is null (tests)
};
