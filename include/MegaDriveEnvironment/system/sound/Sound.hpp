#pragma once

#include "data_types.hpp"
#include "system/sound/mame_ymfm/ymfm.h"
#include "system/sound/mame_ymfm/ymfm_opn.h"

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class MegaDriveEnvironment;

class Sound {
    friend struct SoundPSGTestAccess;

    public:
    static constexpr int kSampleRate = 48000;

    explicit Sound(MegaDriveEnvironment *env);
    ~Sound();

    Sound(const Sound &)            = delete;
    Sound &operator=(const Sound &) = delete;

    void start();
    void stop();

    /// Disables the sound subsystem entirely (--silent): start() opens no
    /// audio device, chip writes are dropped, and status reads return 0.
    void disable() {
        disabled_.store(true, std::memory_order_release);
    }
    bool disabled() const {
        return disabled_.load(std::memory_order_acquire);
    }

    m_byte readYM2612(int port);
    m_byte readYM2612At(uint64_t masterCycles, int port);
    void   writeYM2612(int port, m_byte value);
    void   writeYM2612At(uint64_t masterCycles, int port, m_byte value);
    void   writePSG(m_byte value);
    void   writePSGAt(uint64_t masterCycles, m_byte value);

    /// Shared audio timeline: master cycles elapsed on the wall clock since
    /// this Sound was constructed. Every producer (Z80, 68K) stamps its
    /// port writes with this clock and the renderer chases it, so event
    /// timestamps and rendered samples stay on one timeline.
    uint64_t masterCyclesNow() const;

    struct Diagnostics {
        uint64_t audioFramesRendered = 0;
        uint64_t underruns           = 0;
        uint64_t overruns            = 0;
        uint64_t ymTimerExpirations  = 0;
        uint64_t queuedEvents        = 0;
        uint64_t lateEvents          = 0;
        uint64_t droppedEvents       = 0;
        uint64_t contentionDrops     = 0;
        uint64_t queueFullDrops      = 0;
        uint64_t unavailableDrops    = 0;
        uint64_t clippedSamples      = 0;
        int32_t  peakLeft            = 0;
        int32_t  peakRight           = 0;
        size_t   ringBufferedFrames  = 0;
        size_t   psgRingBufferedFrames = 0;
        uint32_t fmSourceSampleRate  = 0;
    };

    void        resetForDiagnostics();
    void        renderForDiagnostics(int16_t *dst, int frames);
    Diagnostics diagnostics() const;

    // Kept for old callers; audio production is driven by the 48 kHz audio timer.
    void endFrame() {
    }

    private:
    class YMInterface : public ymfm::ymfm_interface {
        public:
        void     resetTiming();
        void     setMasterCycle(uint64_t masterCycle);
        void     syncTimersToMasterCycle(uint64_t masterCycle);
        void     ymfm_sync_mode_write(uint8_t data) override;
        void     ymfm_sync_check_interrupts() override;
        void     ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override;
        void     ymfm_set_busy_end(uint32_t clocks) override;
        bool     ymfm_is_busy() override;
        void     ymfm_update_irq(bool asserted) override;
        bool     irqAsserted() const;
        uint64_t timerExpirationCount() const;

        private:
        std::array<uint64_t, 2> timerDeadlineMasterCycles_{};
        std::array<bool, 2>     timerActive_{};
        uint64_t                busyUntilMasterCycle_ = 0;
        uint64_t                currentMasterCycle_   = 0;
        std::atomic<uint64_t>   timerExpirations_{0};
        bool                    irq_                  = false;
        bool                    syncingTimers_        = false;
    };

    /// SN76489A-compatible PSG matching Mega Drive integrated (ASIC) behaviour.
    /// Cycle-accurate on the master-clock timeline with sample-period integration
    /// (band-limiting equivalent to a box filter per output sample).
    /// In realtime mode the chip state is owned exclusively by the PSG thread.
    struct PSG {
        /// Master cycles per tone/noise half-period unit: (MD master/15)/16.
        static constexpr int kTickCycles = 15 * 16;
        /// Default host preamp (GPX ~1.5x PSG vs FM balance on VA4 MD1).
        static constexpr int kDefaultPreamp = 150;

        void               reset();
        /// Snap the chip timeline to `masterCycle` without emitting audio
        /// (used after reset or host-clock resync).
        void               resync(uint64_t masterCycle);
        void               write(m_byte value);
        /// Advance chip from its current time to `masterCycle` and return the
        /// average stereo level over that span (anti-aliased square/noise).
        std::array<int, 2> renderUntil(uint64_t masterCycle);
        void               setPanning(uint8_t mask);
        void               setPreamp(int percent);

        uint64_t time() const {
            return time_;
        }

        private:
        void               updateToneFreq(int channel, int period);
        void               updateNoiseFreq();
        void               setChannelVolume(int channel, int attenuation);
        void               recomputeChannelOut(int channel);
        std::array<int, 2> mixedLevel() const;
        uint64_t           integrateTone(int channel, uint64_t masterCycle);
        uint64_t           integrateNoise(uint64_t masterCycle);

        uint64_t time_ = 0; ///< last committed master-cycle time

        int latch_           = 3;           ///< power-on: tone #2 attenuation (315-5313A)
        int zeroFreqInc_     = kTickCycles; ///< integrated ASIC: period 0 ≡ 1
        int noiseShiftWidth_ = 15;          ///< 16-bit LFSR (integrated)
        int noiseBitMask_    = 0x9;         ///< XOR taps 0 and 3 (integrated)

        std::array<int, 8>      regs_{};     ///< tone periods / noise ctrl / volumes
        std::array<int, 4>      freqInc_{};  ///< master cycles between polarity flips
        std::array<uint64_t, 4> nextEdge_{}; ///< absolute master cycle of next flip
        std::array<int, 4>      polarity_{}; ///< ±1 square generators
        std::array<int, 4>      volume_{};   ///< linear amplitude from 4-bit atten
        std::array<int, 4>      chanAmpL_{}; ///< left amp percent (preamp × pan)
        std::array<int, 4>      chanAmpR_{}; ///< right amp percent
        std::array<int, 4>      chanOutL_{}; ///< volume × left amp
        std::array<int, 4>      chanOutR_{}; ///< volume × right amp
        int                     noiseShift_ = 0x8000;
        int                     preamp_     = kDefaultPreamp;
        uint8_t                 panMask_    = 0xFF;
    };

    enum class EventType : uint8_t {
        YMWrite,
        PSGWrite,
    };

    struct TimedEvent {
        uint64_t  masterCycle   = 0;
        EventType type          = EventType::YMWrite;
        uint8_t   port          = 0;
        uint8_t   value         = 0;
        bool      timerRegister = false; ///< YM data write hitting $24–$27 (affects polled status)
    };

    static constexpr std::size_t kEventQueueCapacity = 16'384;

    /// Bounded multi-producer/single-consumer queue used only while audio is
    /// streaming. Producers never enter the audio callback's mutex.
    class RealtimeEventQueue {
        public:
        RealtimeEventQueue();

        bool        tryPush(const TimedEvent &event);
        void        drainTo(std::vector<TimedEvent> &destination);
        void        reset();
        std::size_t approximateSize() const;

        private:
        struct Slot {
            std::atomic<std::size_t> sequence{0};
            TimedEvent               event{};
        };

        std::unique_ptr<Slot[]> slots_;
        std::atomic<std::size_t> enqueuePosition_{0};
        std::size_t              dequeuePosition_ = 0;
        std::atomic<std::size_t> publishedDequeuePosition_{0};
    };

    static void        audioCallback(void *userdata, SDL_AudioStream *stream, int additionalAmount, int totalAmount);
    static int         psgThreadEntry(void *userdata);
    void               psgThreadMain();
    void               startPsgThread();
    void               stopPsgThread();
    void               resetChipState();
    void               renderToStream(SDL_AudioStream *stream, int bytesRequested);
    void               ensureRingFrames(int frames);
    void               pushRingFrames(const int16_t *src, int frames);
    void               popRingFrames(int16_t *dst, int frames);
    void               renderSamples(int16_t *dst, int frames);
    void               renderPsgChunk(int frames);
    bool               enqueueYMEvent(TimedEvent event);
    bool               enqueuePSGEvent(TimedEvent event);
    void               prepareYMEvent(TimedEvent &event);
    void               preparePSGEvent(TimedEvent &event);
    void               drainQueueUntil(std::vector<TimedEvent> &queue,
                                       uint64_t                 masterCycle,
                                       std::vector<TimedEvent> &out);
    void               processEventsUntil(uint64_t masterCycle);
    void               applyYMEvent(const TimedEvent &event);
    void               applyPSGEvent(const TimedEvent &event);
    std::array<int, 2> renderFM();
    std::array<int, 2> filterOutput(std::array<int, 2> sample);
    int16_t            clampMixedSample(int value, size_t channel);
    void               setYMQueuedCount(size_t n);
    void               setPSGQueuedCount(size_t n);

    // Threading model while streaming:
    // - Gameplay producers never wait: realtime writes use bounded MPSC queues.
    // - YM2612 chip state + FM render live on the SDL audio callback thread.
    // - PSG chip state + PSG render live on a dedicated "md-psg" worker thread
    //   that fills a SPSC ring; the audio callback only pops and mixes.
    // - Before start() (headless diagnostics), both chips stay synchronous on
    //   the calling thread for deterministic tests.
    MegaDriveEnvironment *env_              = nullptr;
    SDL_Mutex            *mutex_            = nullptr; ///< YM event queue
    SDL_Mutex            *psgMutex_         = nullptr; ///< PSG event queue
    SDL_Thread           *psgThread_        = nullptr;
    SDL_AudioStream      *stream_           = nullptr;
    bool                  audioInitialized_ = false;
    std::atomic<bool>     disabled_{false};
    std::atomic<bool>     realtimeMode_{false};
    std::atomic<bool>     consumerAvailable_{false}; ///< SDL audio callback is draining YM + mix
    std::atomic<bool>     psgThreadRun_{false};      ///< dedicated PSG worker is live
    std::atomic<uint8_t>  cachedStatus_{0};

    YMInterface                         ymInterface_;
    ymfm::ym2612                        ym_;
    PSG                                 psg_;
    ymfm::ym2612::output_data           lastFM_{};
    ymfm::ym2612::output_data           previousFM_{};
    ymfm::ym2612::output_data           nextFM_{};
    double                              fmAccumulator_        = 1.0;
    uint32_t                            fmSampleRate_         = 0;
    double                              renderMasterCycle_    = 0.0; ///< FM/audio timeline
    double                              psgRenderMasterCycle_ = 0.0; ///< PSG worker timeline
    std::atomic<uint64_t>               lastYMRenderedMasterCycle_{0};
    std::atomic<uint64_t>               lastPSGRenderedMasterCycle_{0};
    uint64_t                            baseTimeNS_      = 0;
    std::atomic<uint8_t>                queuedYMAddress_{0};
    std::atomic<uint64_t>               lateEventCount_{0};
    std::atomic<uint64_t>               contentionDropCount_{0};
    std::atomic<uint64_t>               queueFullDropCount_{0};
    std::atomic<uint64_t>               unavailableDropCount_{0};
    std::atomic<size_t>                 ymQueuedEventCount_{0};
    std::atomic<size_t>                 psgQueuedEventCount_{0};
    std::atomic<size_t>                 queuedEventCount_{0};
    std::atomic<uint64_t>               clippedSampleCount_{0};
    std::array<std::atomic<int32_t>, 2> peakSample_{};
    std::array<double, 2>               dcPrevInput_{};
    std::array<double, 2>               dcPrevOutput_{};
    std::array<double, 2>               lowpassState_{};
    std::vector<TimedEvent>             pendingYMEvents_;
    std::vector<TimedEvent>             pendingPSGEvents_;
    RealtimeEventQueue                  realtimeYMEvents_;
    RealtimeEventQueue                  realtimePSGEvents_;
    std::vector<TimedEvent>             renderEvents_;
    std::vector<TimedEvent>             psgRenderEvents_;
    std::vector<int16_t>                callbackBuffer_;
    std::vector<int16_t>                renderBuffer_;
    std::vector<int16_t>                ringBuffer_;
    size_t                              ringReadFrame_      = 0;
    size_t                              ringWriteFrame_     = 0;
    size_t                              ringBufferedFrames_ = 0;
    std::atomic<size_t>                 ringBufferedFramesSnapshot_{0};
    // SPSC PSG sample ring: written by PSG thread, read by audio callback.
    std::vector<int>                    psgRing_;
    size_t                              psgRingReadFrame_  = 0;
    size_t                              psgRingWriteFrame_ = 0;
    std::atomic<size_t>                 psgRingBuffered_{0};
    std::atomic<size_t>                 psgRingBufferedSnapshot_{0};
    std::atomic<uint64_t>               psgUnderrunCount_{0};
    std::atomic<uint64_t>               psgOverrunCount_{0};
    std::atomic<uint64_t>               audioFramesRendered_{0};
    std::atomic<uint64_t>               underrunCount_{0};
    std::atomic<uint64_t>               overrunCount_{0};
};
