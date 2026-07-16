#include "Sound.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr uint32_t kYM2612Clock   = 53'693'175u / 7u;
constexpr uint64_t kMasterClockHz = 53'693'175ull;
constexpr double   kMasterClock   = 53'693'175.0;
// A small scheduling margin keeps producer events in the renderer's future.
// SDL and the physical device add their own buffering, so a larger software
// margin makes music and effects perceptibly trail the game. Inaccurate late
// events are preferable to making gameplay/audio response feel sluggish.
constexpr double   kEventLatencyCycles = 0.012 * kMasterClock;
constexpr double   kSnapCycles         = 0.250 * kMasterClock; // resync hard beyond this drift
constexpr double   kMaxRateTrim        = 0.005;                // ±0.5% render-rate trim toward the latency target
constexpr int      kRenderChunkFrames = 256;
constexpr int      kRingBufferFrames  = 4096;
constexpr int      kPsgRingFrames     = 4096;
constexpr int      kPsgTargetFrames   = 1024; ///< PSG thread keeps this many frames ahead
constexpr size_t   kMaxPendingEvents  = 16'384;
constexpr int      kFmPreampPercent   = 100;
constexpr uint32_t kLowpassRange      = 0x9999;
constexpr double   kDCBlockR          = 0.995;

// Temporary diagnostics: SOR_SND_TAP=<path> dumps rendered s16 stereo frames,
// SOR_YM_LOG=<path> logs every enqueued chip write with its producer thread.
FILE *sndTapFile() {
    static FILE *f = [] {
        const char *p = std::getenv("SOR_SND_TAP");
        return p ? std::fopen(p, "wb") : nullptr;
    }();
    return f;
}

FILE *ymLogFile() {
    static FILE *f = [] {
        const char *p = std::getenv("SOR_YM_LOG");
        return p ? std::fopen(p, "w") : nullptr;
    }();
    return f;
}

int16_t clamp16(int value) {
    return static_cast<int16_t>(std::clamp(value, -32768, 32767));
}

uint64_t ymClocksToMasterCycles(uint32_t clocks) {
    return static_cast<uint64_t>(clocks) * 7ull;
}

int applyPreamp(int value, int percent) {
    return (value * percent) / 100;
}

// 2 dB steps from full scale, matched to Genesis Plus GX / SN76489A tables.
// PSG_MAX_VOLUME 2800 with ~1.5x host preamp balances against ymfm on VA4 MD1.
int psgVolume(uint8_t attenuation) {
    static constexpr std::array<int, 16> kVolume = {
        2800, //  MAX
        2224, // -2 dB
        1767, // -4 dB
        1403, // -6 dB
        1115, // -8 dB
        886,  // -10 dB
        704,  // -12 dB
        559,  // -14 dB
        444,  // -16 dB
        353,  // -18 dB
        280,  // -20 dB
        222,  // -22 dB
        177,  // -24 dB
        140,  // -26 dB
        111,  // -28 dB
        0,    // OFF
    };
    return kVolume[attenuation & 0x0F];
}

// White-noise XOR of the tapped bits (mask selects which LFSR bits feed back).
// For the integrated ASIC, mask is 0x9 → bits 0 and 3.
int noiseFeedbackBit(int shiftValue, int bitMask) {
    const int masked = shiftValue & bitMask;
    int       parity = 0;
    for (int bit = masked; bit != 0; bit >>= 1)
        parity ^= (bit & 1);
    return parity;
}

} // namespace

Sound::Sound(MegaDriveEnvironment *env)
    : env_(env), mutex_(SDL_CreateMutex()), psgMutex_(SDL_CreateMutex()), ym_(ymInterface_) {
    baseTimeNS_   = SDL_GetTicksNS();
    fmSampleRate_ = ym_.sample_rate(kYM2612Clock);
    pendingYMEvents_.reserve(kMaxPendingEvents);
    pendingPSGEvents_.reserve(kMaxPendingEvents);
    renderEvents_.reserve(kMaxPendingEvents);
    psgRenderEvents_.reserve(kMaxPendingEvents);
    psgRing_.assign(static_cast<size_t>(kPsgRingFrames) * 2, 0);
    psg_.reset();
}

uint64_t Sound::masterCyclesNow() const {
    const uint64_t ns  = SDL_GetTicksNS() - baseTimeNS_;
    const uint64_t s   = ns / 1'000'000'000ull;
    const uint64_t rem = ns % 1'000'000'000ull;
    return (s * kMasterClockHz) + ((rem * kMasterClockHz) / 1'000'000'000ull);
}

Sound::~Sound() {
    stop();
    if (mutex_) {
        SDL_DestroyMutex(mutex_);
        mutex_ = nullptr;
    }
    if (psgMutex_) {
        SDL_DestroyMutex(psgMutex_);
        psgMutex_ = nullptr;
    }
}

void Sound::start() {
    if (disabled())
        return;

    // Set this before touching shared sound state. From this point onward a
    // producer may lose a write, but it can never wait for audio startup,
    // rendering, device failure, or shutdown.
    realtimeMode_.store(true, std::memory_order_release);

    if (stream_)
        return;

    SDL_LockMutex(mutex_);
    SDL_LockMutex(psgMutex_);
    resetChipState();
    SDL_UnlockMutex(psgMutex_);
    SDL_UnlockMutex(mutex_);

    // PSG worker consumes writes and fills its ring before the device opens so
    // the first audio callback already has PSG samples to mix.
    startPsgThread();

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "Sound: could not initialize SDL audio: %s\n", SDL_GetError());
        stopPsgThread();
        return;
    }
    audioInitialized_ = true;

    SDL_AudioSpec spec{};
    spec.format   = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq     = kSampleRate;

    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audioCallback, this);
    if (!stream_) {
        std::fprintf(stderr, "Sound: could not open SDL audio stream: %s\n", SDL_GetError());
        stopPsgThread();
        return;
    }
    consumerAvailable_.store(true, std::memory_order_release);
    SDL_ResumeAudioStreamDevice(stream_);
}

void Sound::stop() {
    consumerAvailable_.store(false, std::memory_order_release);
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    stopPsgThread();
    if (audioInitialized_) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audioInitialized_ = false;
    }
    realtimeMode_.store(false, std::memory_order_release);
}

int Sound::psgThreadEntry(void *userdata) {
    static_cast<Sound *>(userdata)->psgThreadMain();
    return 0;
}

void Sound::startPsgThread() {
    if (psgThread_)
        return;
    psgThreadRun_.store(true, std::memory_order_release);
    psgThread_ = SDL_CreateThread(psgThreadEntry, "md-psg", this);
    if (!psgThread_) {
        psgThreadRun_.store(false, std::memory_order_release);
        std::fprintf(stderr, "Sound: could not create PSG thread: %s\n", SDL_GetError());
    }
}

void Sound::stopPsgThread() {
    psgThreadRun_.store(false, std::memory_order_release);
    if (psgThread_) {
        SDL_WaitThread(psgThread_, nullptr);
        psgThread_ = nullptr;
    }
}

void Sound::psgThreadMain() {
    while (psgThreadRun_.load(std::memory_order_acquire)) {
        const size_t buffered = psgRingBuffered_.load(std::memory_order_relaxed);
        if (buffered >= static_cast<size_t>(kPsgTargetFrames)) {
            SDL_DelayNS(500'000); // 0.5 ms — avoid busy-spin when ahead
            continue;
        }

        const size_t freeFrames = static_cast<size_t>(kPsgRingFrames) - buffered;
        if (freeFrames == 0) {
            SDL_DelayNS(500'000);
            continue;
        }

        const int chunk = std::min<int>(kRenderChunkFrames, static_cast<int>(freeFrames));
        renderPsgChunk(chunk);
    }
}

void Sound::renderPsgChunk(int frames) {
    if (frames <= 0)
        return;

    const double target = static_cast<double>(masterCyclesNow()) - kEventLatencyCycles;
    double       error  = psgRenderMasterCycle_ - target;
    if (std::abs(error) > kSnapCycles) {
        psgRenderMasterCycle_ = std::max(target, 0.0);
        psg_.resync(static_cast<uint64_t>(psgRenderMasterCycle_));
        error = 0.0;
    }
    const double step = (kMasterClock / static_cast<double>(kSampleRate)) *
                        (1.0 - std::clamp(error / kSnapCycles, -kMaxRateTrim, kMaxRateTrim));

    psgRenderEvents_.clear();
    const uint64_t chunkLastCycle =
        static_cast<uint64_t>(psgRenderMasterCycle_ + (step * static_cast<double>(frames - 1)));

    bool locked = false;
    if (realtimeMode_.load(std::memory_order_acquire))
        locked = SDL_TryLockMutex(psgMutex_);
    else {
        SDL_LockMutex(psgMutex_);
        locked = true;
    }
    if (locked) {
        drainQueueUntil(pendingPSGEvents_, chunkLastCycle, psgRenderEvents_);
        setPSGQueuedCount(pendingPSGEvents_.size());
        SDL_UnlockMutex(psgMutex_);
        std::stable_sort(psgRenderEvents_.begin(), psgRenderEvents_.end(), [](const TimedEvent &a, const TimedEvent &b) {
            return a.masterCycle < b.masterCycle;
        });
    }

    size_t nextEvent = 0;
    for (int i = 0; i < frames; ++i) {
        const uint64_t masterCycle = static_cast<uint64_t>(psgRenderMasterCycle_);
        while (nextEvent < psgRenderEvents_.size() && psgRenderEvents_[nextEvent].masterCycle <= masterCycle)
            applyPSGEvent(psgRenderEvents_[nextEvent++]);

        const auto sample = psg_.renderUntil(masterCycle);
        if (!pushPsgFrame(sample[0], sample[1]))
            psgOverrunCount_.fetch_add(1, std::memory_order_relaxed);

        lastPSGRenderedMasterCycle_.store(masterCycle, std::memory_order_relaxed);
        psgRenderMasterCycle_ += step;
    }
}

bool Sound::pushPsgFrame(int left, int right) {
    const size_t capacity = static_cast<size_t>(kPsgRingFrames);
    if (psgRingBuffered_.load(std::memory_order_relaxed) >= capacity)
        return false;
    if (psgRing_.size() < capacity * 2)
        psgRing_.assign(capacity * 2, 0);

    psgRing_[psgRingWriteFrame_ * 2 + 0] = left;
    psgRing_[psgRingWriteFrame_ * 2 + 1] = right;
    psgRingWriteFrame_                   = (psgRingWriteFrame_ + 1) % capacity;
    psgRingBuffered_.fetch_add(1, std::memory_order_release);
    psgRingBufferedSnapshot_.store(psgRingBuffered_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return true;
}

bool Sound::popPsgFrame(int &left, int &right) {
    if (psgRingBuffered_.load(std::memory_order_acquire) == 0)
        return false;
    const size_t capacity                = static_cast<size_t>(kPsgRingFrames);
    left                                 = psgRing_[psgRingReadFrame_ * 2 + 0];
    right                                = psgRing_[psgRingReadFrame_ * 2 + 1];
    psgRingReadFrame_                    = (psgRingReadFrame_ + 1) % capacity;
    psgRingBuffered_.fetch_sub(1, std::memory_order_release);
    psgRingBufferedSnapshot_.store(psgRingBuffered_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return true;
}

m_byte Sound::readYM2612(int port) {
    return readYM2612At(masterCyclesNow(), port);
}

m_byte Sound::readYM2612At(uint64_t masterCycles, int port) {
    (void)port; // every YM2612 port reads back the status register
    if (disabled())
        return 0;

    // While streaming, the chip belongs to the audio thread; status reads
    // must never block gameplay on it, so they return the last status the
    // renderer published (a few ms stale at worst — the busy-wait loops in
    // the drivers only need "not busy").
    if (realtimeMode_.load(std::memory_order_acquire))
        return cachedStatus_.load(std::memory_order_relaxed);

    SDL_LockMutex(mutex_);
    SDL_LockMutex(psgMutex_);
    processEventsUntil(masterCycles);
    SDL_UnlockMutex(psgMutex_);
    ymInterface_.syncTimersToMasterCycle(masterCycles);
    m_byte result = ym_.read(0);
    SDL_UnlockMutex(mutex_);
    return result;
}

void Sound::writeYM2612(int port, m_byte value) {
    writeYM2612At(masterCyclesNow(), port, value);
}

void Sound::writeYM2612At(uint64_t masterCycles, int port, m_byte value) {
    if (disabled())
        return;

    TimedEvent event{
        .masterCycle = masterCycles,
        .type        = EventType::YMWrite,
        .port        = static_cast<uint8_t>(port & 3),
        .value       = value,
    };
    if (realtimeMode_.load(std::memory_order_acquire)) {
        if (!consumerAvailable_.load(std::memory_order_acquire)) {
            unavailableDropCount_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!SDL_TryLockMutex(mutex_)) {
            contentionDropCount_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        enqueueYMEvent(event);
        SDL_UnlockMutex(mutex_);
        return;
    }

    SDL_LockMutex(mutex_);
    SDL_LockMutex(psgMutex_);
    if (enqueueYMEvent(event))
        processEventsUntil(masterCycles);
    SDL_UnlockMutex(psgMutex_);
    SDL_UnlockMutex(mutex_);
}

void Sound::writePSG(m_byte value) {
    writePSGAt(masterCyclesNow(), value);
}

void Sound::writePSGAt(uint64_t masterCycles, m_byte value) {
    if (disabled())
        return;

    TimedEvent event{
        .masterCycle = masterCycles,
        .type        = EventType::PSGWrite,
        .port        = 0,
        .value       = value,
    };
    if (realtimeMode_.load(std::memory_order_acquire)) {
        // PSG writes are consumed by the dedicated PSG thread, not the audio callback.
        if (!psgThreadRun_.load(std::memory_order_acquire)) {
            unavailableDropCount_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!SDL_TryLockMutex(psgMutex_)) {
            contentionDropCount_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        enqueuePSGEvent(event);
        SDL_UnlockMutex(psgMutex_);
        return;
    }

    SDL_LockMutex(mutex_);
    SDL_LockMutex(psgMutex_);
    if (enqueuePSGEvent(event))
        processEventsUntil(masterCycles);
    SDL_UnlockMutex(psgMutex_);
    SDL_UnlockMutex(mutex_);
}

void Sound::resetForDiagnostics() {
    SDL_LockMutex(mutex_);
    SDL_LockMutex(psgMutex_);
    resetChipState();
    SDL_UnlockMutex(psgMutex_);
    SDL_UnlockMutex(mutex_);
}

void Sound::renderForDiagnostics(int16_t *dst, int frames) {
    renderSamples(dst, frames);
}

Sound::Diagnostics Sound::diagnostics() const {
    const uint64_t contentionDrops  = contentionDropCount_.load(std::memory_order_relaxed);
    const uint64_t queueFullDrops   = queueFullDropCount_.load(std::memory_order_relaxed);
    const uint64_t unavailableDrops = unavailableDropCount_.load(std::memory_order_relaxed);
    const uint64_t psgUnderruns     = psgUnderrunCount_.load(std::memory_order_relaxed);
    const uint64_t psgOverruns      = psgOverrunCount_.load(std::memory_order_relaxed);
    return Diagnostics{
        .audioFramesRendered   = audioFramesRendered_.load(std::memory_order_relaxed),
        .underruns             = underrunCount_.load(std::memory_order_relaxed) + psgUnderruns,
        .overruns              = overrunCount_.load(std::memory_order_relaxed) + psgOverruns,
        .ymTimerExpirations    = ymInterface_.timerExpirationCount(),
        .queuedEvents          = queuedEventCount_.load(std::memory_order_relaxed),
        .lateEvents            = lateEventCount_.load(std::memory_order_relaxed),
        .droppedEvents         = contentionDrops + queueFullDrops + unavailableDrops,
        .contentionDrops       = contentionDrops,
        .queueFullDrops        = queueFullDrops,
        .unavailableDrops      = unavailableDrops,
        .clippedSamples        = clippedSampleCount_.load(std::memory_order_relaxed),
        .peakLeft              = peakSample_[0].load(std::memory_order_relaxed),
        .peakRight             = peakSample_[1].load(std::memory_order_relaxed),
        .ringBufferedFrames    = ringBufferedFramesSnapshot_.load(std::memory_order_relaxed),
        .psgRingBufferedFrames = psgRingBufferedSnapshot_.load(std::memory_order_relaxed),
        .fmSourceSampleRate    = fmSampleRate_,
    };
}

void Sound::audioCallback(void *userdata, SDL_AudioStream *stream, int additionalAmount, int totalAmount) {
    (void)totalAmount;
    static_cast<Sound *>(userdata)->renderToStream(stream, additionalAmount);
}

void Sound::resetChipState() {
    ymInterface_.resetTiming();
    ym_.reset();
    psg_.reset();
    lastFM_.clear();
    previousFM_.clear();
    nextFM_.clear();
    ym_.generate(&previousFM_, 1);
    ym_.generate(&nextFM_, 1);
    fmAccumulator_            = 1.0;
    renderMasterCycle_        = std::max(0.0, static_cast<double>(masterCyclesNow()) - kEventLatencyCycles);
    psgRenderMasterCycle_     = renderMasterCycle_;
    psg_.resync(static_cast<uint64_t>(psgRenderMasterCycle_));
    lastYMRenderedMasterCycle_.store(0, std::memory_order_relaxed);
    lastPSGRenderedMasterCycle_.store(0, std::memory_order_relaxed);
    queuedYMAddress_ = 0;
    lateEventCount_.store(0, std::memory_order_relaxed);
    contentionDropCount_.store(0, std::memory_order_relaxed);
    queueFullDropCount_.store(0, std::memory_order_relaxed);
    unavailableDropCount_.store(0, std::memory_order_relaxed);
    clippedSampleCount_.store(0, std::memory_order_relaxed);
    peakSample_[0].store(0, std::memory_order_relaxed);
    peakSample_[1].store(0, std::memory_order_relaxed);
    cachedStatus_.store(0, std::memory_order_relaxed);
    dcPrevInput_.fill(0.0);
    dcPrevOutput_.fill(0.0);
    lowpassState_.fill(0.0);
    pendingYMEvents_.clear();
    pendingPSGEvents_.clear();
    renderEvents_.clear();
    psgRenderEvents_.clear();
    ymQueuedEventCount_.store(0, std::memory_order_relaxed);
    psgQueuedEventCount_.store(0, std::memory_order_relaxed);
    queuedEventCount_.store(0, std::memory_order_relaxed);
    ringBuffer_.assign(kRingBufferFrames * 2, 0);
    ringReadFrame_      = 0;
    ringWriteFrame_     = 0;
    ringBufferedFrames_ = 0;
    ringBufferedFramesSnapshot_.store(0, std::memory_order_relaxed);
    psgRing_.assign(static_cast<size_t>(kPsgRingFrames) * 2, 0);
    psgRingReadFrame_  = 0;
    psgRingWriteFrame_ = 0;
    psgRingBuffered_.store(0, std::memory_order_relaxed);
    psgRingBufferedSnapshot_.store(0, std::memory_order_relaxed);
    psgUnderrunCount_.store(0, std::memory_order_relaxed);
    psgOverrunCount_.store(0, std::memory_order_relaxed);
    audioFramesRendered_.store(0, std::memory_order_relaxed);
    underrunCount_.store(0, std::memory_order_relaxed);
    overrunCount_.store(0, std::memory_order_relaxed);
    ymInterface_.resetTiming();
}

void Sound::setYMQueuedCount(size_t n) {
    ymQueuedEventCount_.store(n, std::memory_order_relaxed);
    queuedEventCount_.store(n + psgQueuedEventCount_.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void Sound::setPSGQueuedCount(size_t n) {
    psgQueuedEventCount_.store(n, std::memory_order_relaxed);
    queuedEventCount_.store(ymQueuedEventCount_.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

void Sound::renderToStream(SDL_AudioStream *stream, int bytesRequested) {
    if (bytesRequested <= 0)
        return;
    const int frames =
        (bytesRequested + static_cast<int>(sizeof(int16_t) * 2) - 1) / static_cast<int>(sizeof(int16_t) * 2);
    callbackBuffer_.resize(static_cast<size_t>(frames) * 2);
    ensureRingFrames(frames);
    popRingFrames(callbackBuffer_.data(), frames);
    SDL_PutAudioStreamData(stream, callbackBuffer_.data(), static_cast<int>(callbackBuffer_.size() * sizeof(int16_t)));
}

void Sound::ensureRingFrames(int frames) {
    if (frames <= 0)
        return;

    if (ringBuffer_.empty()) {
        ringBuffer_.assign(std::max(kRingBufferFrames, frames) * 2, 0);
        ringReadFrame_      = 0;
        ringWriteFrame_     = 0;
        ringBufferedFrames_ = 0;
        ringBufferedFramesSnapshot_.store(0, std::memory_order_relaxed);
    }

    const size_t capacityFrames = ringBuffer_.size() / 2;
    while (ringBufferedFrames_ < static_cast<size_t>(frames)) {
        const size_t freeFrames = capacityFrames - ringBufferedFrames_;
        if (freeFrames == 0) {
            ++overrunCount_;
            break;
        }

        const int chunkFrames = std::min<int>(kRenderChunkFrames, static_cast<int>(freeFrames));
        renderBuffer_.resize(static_cast<size_t>(chunkFrames) * 2);
        renderSamples(renderBuffer_.data(), chunkFrames);
        pushRingFrames(renderBuffer_.data(), chunkFrames);
    }
}

void Sound::pushRingFrames(const int16_t *src, int frames) {
    const size_t capacityFrames = ringBuffer_.size() / 2;
    for (int frame = 0; frame < frames; ++frame) {
        if (ringBufferedFrames_ >= capacityFrames) {
            ++overrunCount_;
            return;
        }

        const size_t dstFrame         = ringWriteFrame_;
        ringBuffer_[dstFrame * 2 + 0] = src[frame * 2 + 0];
        ringBuffer_[dstFrame * 2 + 1] = src[frame * 2 + 1];
        ringWriteFrame_               = (ringWriteFrame_ + 1) % capacityFrames;
        ++ringBufferedFrames_;
    }
    ringBufferedFramesSnapshot_.store(ringBufferedFrames_, std::memory_order_relaxed);
}

void Sound::popRingFrames(int16_t *dst, int frames) {
    const size_t capacityFrames = ringBuffer_.size() / 2;
    for (int frame = 0; frame < frames; ++frame) {
        if (ringBufferedFrames_ == 0 || capacityFrames == 0) {
            ++underrunCount_;
            dst[frame * 2 + 0] = 0;
            dst[frame * 2 + 1] = 0;
            continue;
        }

        const size_t srcFrame = ringReadFrame_;
        dst[frame * 2 + 0]    = ringBuffer_[srcFrame * 2 + 0];
        dst[frame * 2 + 1]    = ringBuffer_[srcFrame * 2 + 1];
        ringReadFrame_        = (ringReadFrame_ + 1) % capacityFrames;
        --ringBufferedFrames_;
    }
    ringBufferedFramesSnapshot_.store(ringBufferedFrames_, std::memory_order_relaxed);
}

void Sound::renderSamples(int16_t *dst, int frames) {
    const bool psgThreaded = psgThreadRun_.load(std::memory_order_acquire);

    for (int base = 0; base < frames; base += kRenderChunkFrames) {
        const int chunkFrames = std::min(kRenderChunkFrames, frames - base);
        // Keep the FM render clock kEventLatencyCycles behind the producers'
        // wall clock: snap on gross drift (startup, host stalls), otherwise
        // trim the per-sample step so the pitch shift stays inaudible.
        const double target = static_cast<double>(masterCyclesNow()) - kEventLatencyCycles;
        double       error  = renderMasterCycle_ - target;
        if (std::abs(error) > kSnapCycles) {
            renderMasterCycle_ = std::max(target, 0.0);
            // PSG timeline is owned by its own thread (or by the headless path
            // below); only resync the local chip when rendering PSG inline.
            if (!psgThreaded) {
                psgRenderMasterCycle_ = renderMasterCycle_;
                psg_.resync(static_cast<uint64_t>(renderMasterCycle_));
            }
            error = 0.0;
        }
        const double step = (kMasterClock / static_cast<double>(kSampleRate)) *
                            (1.0 - std::clamp(error / kSnapCycles, -kMaxRateTrim, kMaxRateTrim));

        // Drain YM events for this chunk. PSG events are handled on the PSG
        // thread (realtime) or inline below (headless).
        renderEvents_.clear();
        const uint64_t chunkLastCycle =
            static_cast<uint64_t>(renderMasterCycle_ + (step * static_cast<double>(chunkFrames - 1)));
        bool queueLocked = false;
        if (realtimeMode_.load(std::memory_order_acquire))
            queueLocked = SDL_TryLockMutex(mutex_);
        else {
            SDL_LockMutex(mutex_);
            queueLocked = true;
        }
        if (queueLocked) {
            drainQueueUntil(pendingYMEvents_, chunkLastCycle, renderEvents_);
            setYMQueuedCount(pendingYMEvents_.size());
            if (!psgThreaded) {
                // Headless: also pull PSG events so process order stays unified.
                SDL_LockMutex(psgMutex_);
                drainQueueUntil(pendingPSGEvents_, chunkLastCycle, renderEvents_);
                setPSGQueuedCount(pendingPSGEvents_.size());
                SDL_UnlockMutex(psgMutex_);
            }
            SDL_UnlockMutex(mutex_);
            std::stable_sort(renderEvents_.begin(), renderEvents_.end(), [](const TimedEvent &a, const TimedEvent &b) {
                return a.masterCycle < b.masterCycle;
            });
        }

        size_t nextEvent = 0;
        for (int i = 0; i < chunkFrames; ++i) {
            const int      frame       = base + i;
            const uint64_t masterCycle = static_cast<uint64_t>(renderMasterCycle_);
            while (nextEvent < renderEvents_.size() && renderEvents_[nextEvent].masterCycle <= masterCycle) {
                const TimedEvent &event = renderEvents_[nextEvent++];
                if (event.type == EventType::YMWrite)
                    applyYMEvent(event);
                else if (!psgThreaded)
                    applyPSGEvent(event);
            }
            ymInterface_.syncTimersToMasterCycle(masterCycle);
            const auto fm = renderFM();

            std::array<int, 2> psg{0, 0};
            if (psgThreaded) {
                if (!popPsgFrame(psg[0], psg[1])) {
                    psgUnderrunCount_.fetch_add(1, std::memory_order_relaxed);
                    // Hold last level implicitly as silence on underrun.
                }
            } else {
                psg = psg_.renderUntil(masterCycle);
                lastPSGRenderedMasterCycle_.store(masterCycle, std::memory_order_relaxed);
                psgRenderMasterCycle_ = renderMasterCycle_ + step;
            }

            const auto filtered = filterOutput({fm[0] + psg[0], fm[1] + psg[1]});
            dst[frame * 2 + 0]  = clampMixedSample(filtered[0], 0);
            dst[frame * 2 + 1]  = clampMixedSample(filtered[1], 1);
            lastYMRenderedMasterCycle_.store(masterCycle, std::memory_order_relaxed);
            renderMasterCycle_ += step;
        }
        cachedStatus_.store(ym_.read(0), std::memory_order_relaxed);
        audioFramesRendered_.fetch_add(static_cast<uint64_t>(chunkFrames), std::memory_order_relaxed);
        if (FILE *tap = sndTapFile())
            std::fwrite(
                dst + static_cast<size_t>(base) * 2, sizeof(int16_t), static_cast<size_t>(chunkFrames) * 2, tap);
    }
}

bool Sound::enqueueYMEvent(TimedEvent event) {
    if (pendingYMEvents_.size() >= kMaxPendingEvents) {
        queueFullDropCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const uint64_t lastRendered = lastYMRenderedMasterCycle_.load(std::memory_order_relaxed);
    if (event.masterCycle < lastRendered) {
        event.masterCycle = lastRendered;
        lateEventCount_.fetch_add(1, std::memory_order_relaxed);
    }

    if ((event.port & 1u) == 0) {
        if (event.port == 0)
            queuedYMAddress_ = event.value;
    } else if (event.port == 1 && queuedYMAddress_ >= 0x24 && queuedYMAddress_ <= 0x27) {
        // Optimistically clear the timer flags in the published status so
        // a driver that just wrote $27 to reset them doesn't re-read the
        // stale set flags before the renderer applies the write.
        event.timerRegister = true;
        cachedStatus_.fetch_and(static_cast<uint8_t>(~0x03u), std::memory_order_relaxed);
    }

    pendingYMEvents_.push_back(event);
    setYMQueuedCount(pendingYMEvents_.size());
    return true;
}

bool Sound::enqueuePSGEvent(TimedEvent event) {
    if (pendingPSGEvents_.size() >= kMaxPendingEvents) {
        queueFullDropCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const uint64_t lastRendered = lastPSGRenderedMasterCycle_.load(std::memory_order_relaxed);
    if (event.masterCycle < lastRendered) {
        event.masterCycle = lastRendered;
        lateEventCount_.fetch_add(1, std::memory_order_relaxed);
    }

    pendingPSGEvents_.push_back(event);
    setPSGQueuedCount(pendingPSGEvents_.size());
    return true;
}

void Sound::drainQueueUntil(std::vector<TimedEvent> &queue, uint64_t masterCycle, std::vector<TimedEvent> &out) {
    size_t keep = 0;
    for (size_t i = 0; i < queue.size(); ++i) {
        if (queue[i].masterCycle <= masterCycle)
            out.push_back(queue[i]);
        else
            queue[keep++] = queue[i];
    }
    queue.resize(keep);
}

void Sound::processEventsUntil(uint64_t masterCycle) {
    // Headless path: caller holds both mutexes. Apply YM + PSG in timestamp order.
    renderEvents_.clear();
    drainQueueUntil(pendingYMEvents_, masterCycle, renderEvents_);
    drainQueueUntil(pendingPSGEvents_, masterCycle, renderEvents_);
    setYMQueuedCount(pendingYMEvents_.size());
    setPSGQueuedCount(pendingPSGEvents_.size());
    std::stable_sort(renderEvents_.begin(), renderEvents_.end(), [](const TimedEvent &a, const TimedEvent &b) {
        return a.masterCycle < b.masterCycle;
    });
    for (const TimedEvent &event : renderEvents_) {
        if (event.type == EventType::YMWrite)
            applyYMEvent(event);
        else
            applyPSGEvent(event);
    }
}

void Sound::applyYMEvent(const TimedEvent &event) {
    if (FILE *log = ymLogFile()) {
        std::fprintf(log,
                     "Y %llu port=%u val=%02X\n",
                     static_cast<unsigned long long>(event.masterCycle),
                     event.port,
                     event.value);
    }
    ymInterface_.syncTimersToMasterCycle(event.masterCycle);
    ym_.write(event.port & 3u, event.value);
}

void Sound::applyPSGEvent(const TimedEvent &event) {
    if (FILE *log = ymLogFile()) {
        std::fprintf(log,
                     "P %llu port=%u val=%02X\n",
                     static_cast<unsigned long long>(event.masterCycle),
                     event.port,
                     event.value);
    }
    psg_.write(event.value);
}

std::array<int, 2> Sound::renderFM() {
    if (fmSampleRate_ == 0)
        return {0, 0};
    fmAccumulator_ += static_cast<double>(fmSampleRate_);
    while (fmAccumulator_ >= static_cast<double>(kSampleRate)) {
        previousFM_ = nextFM_;
        ym_.generate(&nextFM_, 1);
        fmAccumulator_ -= static_cast<double>(kSampleRate);
    }
    const double frac = fmAccumulator_ / static_cast<double>(kSampleRate);
    lastFM_.data[0]   = static_cast<int32_t>((static_cast<double>(previousFM_.data[0]) * (1.0 - frac)) +
                                             (static_cast<double>(nextFM_.data[0]) * frac));
    lastFM_.data[1]   = static_cast<int32_t>((static_cast<double>(previousFM_.data[1]) * (1.0 - frac)) +
                                             (static_cast<double>(nextFM_.data[1]) * frac));
    return {
        applyPreamp(std::clamp(lastFM_.data[0], -24000, 24000), kFmPreampPercent),
        applyPreamp(std::clamp(lastFM_.data[1], -24000, 24000), kFmPreampPercent),
    };
}

std::array<int, 2> Sound::filterOutput(std::array<int, 2> sample) {
    constexpr uint32_t kLowpassInput = 0x10000u - kLowpassRange;
    std::array<int, 2> out{};
    for (size_t ch = 0; ch < 2; ++ch) {
        const double input = static_cast<double>(sample[ch]);
        const double hp    = input - dcPrevInput_[ch] + (kDCBlockR * dcPrevOutput_[ch]);
        dcPrevInput_[ch]   = input;
        dcPrevOutput_[ch]  = hp;
        lowpassState_[ch] =
            ((lowpassState_[ch] * static_cast<double>(kLowpassRange)) + (hp * static_cast<double>(kLowpassInput))) /
            65536.0;
        out[ch] = static_cast<int>(lowpassState_[ch]);
    }
    return out;
}

int16_t Sound::clampMixedSample(int value, size_t channel) {
    const int32_t absValue = static_cast<int32_t>(std::abs(value));
    if (channel < peakSample_.size() && absValue > peakSample_[channel].load(std::memory_order_relaxed))
        peakSample_[channel].store(absValue, std::memory_order_relaxed); // single writer: render thread
    if (value < -32768 || value > 32767)
        ++clippedSampleCount_;
    return clamp16(value);
}

void Sound::YMInterface::resetTiming() {
    timerDeadlineMasterCycles_.fill(0);
    timerActive_.fill(false);
    busyUntilMasterCycle_ = 0;
    currentMasterCycle_   = 0;
    timerExpirations_.store(0, std::memory_order_relaxed);
    irq_                  = false;
    syncingTimers_        = false;
}

void Sound::YMInterface::setMasterCycle(uint64_t masterCycle) {
    currentMasterCycle_ = masterCycle;
}

void Sound::YMInterface::syncTimersToMasterCycle(uint64_t masterCycle) {
    if (syncingTimers_)
        return;

    currentMasterCycle_ = std::max(currentMasterCycle_, masterCycle);
    syncingTimers_      = true;
    for (int expiredCount = 0; expiredCount < 16; ++expiredCount) {
        int      expiredTimer = -1;
        uint64_t expiredAt    = 0;
        for (size_t t = 0; t < timerActive_.size(); ++t) {
            if (!timerActive_[t] || timerDeadlineMasterCycles_[t] > currentMasterCycle_)
                continue;
            if (expiredTimer < 0 || timerDeadlineMasterCycles_[t] < expiredAt) {
                expiredTimer = static_cast<int>(t);
                expiredAt    = timerDeadlineMasterCycles_[t];
            }
        }

        if (expiredTimer < 0)
            break;

        timerActive_[static_cast<size_t>(expiredTimer)]               = false;
        timerDeadlineMasterCycles_[static_cast<size_t>(expiredTimer)] = 0;
        timerExpirations_.fetch_add(1, std::memory_order_relaxed);
        if (m_engine)
            m_engine->engine_timer_expired(static_cast<uint32_t>(expiredTimer));
    }
    syncingTimers_ = false;
}

void Sound::YMInterface::ymfm_sync_mode_write(uint8_t data) {
    syncTimersToMasterCycle(currentMasterCycle_);
    if (m_engine)
        m_engine->engine_mode_write(data);
}

void Sound::YMInterface::ymfm_sync_check_interrupts() {
    syncTimersToMasterCycle(currentMasterCycle_);
    if (m_engine)
        m_engine->engine_check_interrupts();
}

void Sound::YMInterface::ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) {
    if (tnum >= timerActive_.size())
        return;

    if (duration_in_clocks < 0) {
        timerActive_[tnum]               = false;
        timerDeadlineMasterCycles_[tnum] = 0;
        return;
    }

    timerActive_[tnum] = true;
    timerDeadlineMasterCycles_[tnum] =
        currentMasterCycle_ + ymClocksToMasterCycles(static_cast<uint32_t>(duration_in_clocks));
}

void Sound::YMInterface::ymfm_set_busy_end(uint32_t clocks) {
    busyUntilMasterCycle_ = currentMasterCycle_ + ymClocksToMasterCycles(clocks);
}

bool Sound::YMInterface::ymfm_is_busy() {
    return currentMasterCycle_ < busyUntilMasterCycle_;
}

void Sound::YMInterface::ymfm_update_irq(bool asserted) {
    irq_ = asserted;
}

bool Sound::YMInterface::irqAsserted() const {
    return irq_;
}

uint64_t Sound::YMInterface::timerExpirationCount() const {
    return timerExpirations_.load(std::memory_order_relaxed);
}

void Sound::PSG::reset() {
    // Mega Drive uses the VDP-integrated ASIC clone (not the discrete SN76489).
    zeroFreqInc_     = kTickCycles; // period 0 ≡ 1
    noiseShiftWidth_ = 15;
    noiseBitMask_    = 0x9;
    preamp_          = kDefaultPreamp;
    panMask_         = 0xFF;
    time_            = 0;
    latch_           = 3; // tone #2 attenuation latched on power-on (315-5313A)

    for (int i = 0; i < 4; ++i) {
        regs_[i * 2]     = 0;
        regs_[i * 2 + 1] = 0;
        freqInc_[i]      = (i < 3) ? zeroFreqInc_ : (0x10 * kTickCycles);
        nextEdge_[i]     = 0;
        polarity_[i]     = -1;
        volume_[i]       = 0;
        chanOutL_[i]     = 0;
        chanOutR_[i]     = 0;
    }
    noiseShift_ = 1 << noiseShiftWidth_;
    setPanning(panMask_);
}

void Sound::PSG::resync(uint64_t masterCycle) {
    time_ = masterCycle;
    // Schedule the next polarity flip at the new origin (same as GPX leaving
    // freqCounter at the frame boundary). No silent half-period of lag.
    for (int i = 0; i < 4; ++i)
        nextEdge_[i] = masterCycle;
}

void Sound::PSG::setPreamp(int percent) {
    preamp_ = percent;
    setPanning(panMask_);
}

void Sound::PSG::setPanning(uint8_t mask) {
    panMask_ = mask;
    for (int ch = 0; ch < 4; ++ch) {
        const bool leftOn  = (mask & (1u << (ch + 4))) != 0;
        const bool rightOn = (mask & (1u << ch)) != 0;
        chanAmpL_[ch]      = leftOn ? preamp_ : 0;
        chanAmpR_[ch]      = rightOn ? preamp_ : 0;
        recomputeChannelOut(ch);
    }
}

void Sound::PSG::recomputeChannelOut(int channel) {
    chanOutL_[channel] = (volume_[channel] * chanAmpL_[channel]) / 100;
    chanOutR_[channel] = (volume_[channel] * chanAmpR_[channel]) / 100;
}

void Sound::PSG::setChannelVolume(int channel, int attenuation) {
    volume_[channel]       = psgVolume(static_cast<uint8_t>(attenuation & 0x0F));
    regs_[channel * 2 + 1] = volume_[channel];
    recomputeChannelOut(channel);
}

void Sound::PSG::updateToneFreq(int channel, int period) {
    regs_[channel * 2] = period & 0x3FF;
    if (period != 0)
        freqInc_[channel] = period * kTickCycles;
    else
        freqInc_[channel] = zeroFreqInc_;

    // Noise generator may track tone channel #2 (period register index 4).
    if (channel == 2 && (regs_[6] & 0x03) == 0x03)
        freqInc_[3] = freqInc_[2];
}

void Sound::PSG::updateNoiseFreq() {
    const int noiseFreq = regs_[6] & 0x03;
    if (noiseFreq == 0x03) {
        // Clocked by tone channel #2's generator (same half-period).
        freqInc_[3]  = freqInc_[2];
        nextEdge_[3] = nextEdge_[2];
    } else {
        // Separate rates: N/512, N/1024, N/2048 of the PSG clock.
        // Half-period units are (0x10 << rate); LFSR shifts on the rising edge
        // only, so the shift period is 2 × that (matches hardware / GPX).
        freqInc_[3] = (0x10 << noiseFreq) * kTickCycles;
    }
}

std::array<int, 2> Sound::PSG::mixedLevel() const {
    int left  = 0;
    int right = 0;
    for (int ch = 0; ch < 3; ++ch) {
        if (polarity_[ch] > 0) {
            left += chanOutL_[ch];
            right += chanOutR_[ch];
        }
    }
    if (noiseShift_ & 1) {
        left += chanOutL_[3];
        right += chanOutR_[3];
    }
    return {left, right};
}

uint64_t Sound::PSG::nextEdgeTime() const {
    uint64_t next = nextEdge_[0];
    for (int ch = 1; ch < 4; ++ch)
        next = std::min(next, nextEdge_[ch]);
    return next;
}

void Sound::PSG::processToneEdge(int channel) {
    polarity_[channel] = -polarity_[channel];
    nextEdge_[channel] += static_cast<uint64_t>(std::max(freqInc_[channel], 1));
}

void Sound::PSG::processNoiseEdge() {
    polarity_[3] = -polarity_[3];
    // LFSR advances only on the rising edge of the noise clock.
    if (polarity_[3] > 0) {
        const int shiftOutput = noiseShift_ & 0x01;
        if (regs_[6] & 0x04) {
            // White noise: XOR feedback network.
            const int feedback = noiseFeedbackBit(noiseShift_, noiseBitMask_);
            noiseShift_        = (noiseShift_ >> 1) | (feedback << noiseShiftWidth_);
        } else {
            // Periodic noise: recycle current output bit.
            noiseShift_ = (noiseShift_ >> 1) | (shiftOutput << noiseShiftWidth_);
        }
    }
    nextEdge_[3] += static_cast<uint64_t>(std::max(freqInc_[3], 1));
}

void Sound::PSG::processEdgesAt(uint64_t edgeTime) {
    for (int ch = 0; ch < 3; ++ch) {
        if (nextEdge_[ch] == edgeTime)
            processToneEdge(ch);
    }
    if (nextEdge_[3] == edgeTime)
        processNoiseEdge();
}

void Sound::PSG::write(m_byte value) {
    int index = latch_;
    if (value & 0x80) {
        // Latch register index (1xxx----).
        latch_ = index = (value >> 4) & 0x07;
    }

    switch (index) {
        case 0:
        case 2:
        case 4: {
            // 10-bit tone period: low nibble on latch write, high 6 bits on data.
            int period = regs_[index];
            if (value & 0x80)
                period = (period & 0x3F0) | (value & 0x0F);
            else
                period = (period & 0x00F) | ((value & 0x3F) << 4);
            updateToneFreq(index >> 1, period);
            break;
        }
        case 6: {
            // Noise control: -----fb r r  (fb = white/periodic, rr = rate).
            regs_[6] = value & 0x07;
            updateNoiseFreq();
            // Reset LFSR; output forced low until rising edges refill it.
            noiseShift_ = 1 << noiseShiftWidth_;
            break;
        }
        case 7:
            setChannelVolume(3, value);
            break;
        default:
            // Tone attenuation registers 1, 3, 5.
            setChannelVolume(index >> 1, value);
            break;
    }
}

std::array<int, 2> Sound::PSG::renderUntil(uint64_t masterCycle) {
    if (masterCycle < time_) {
        // Host timeline snapped backwards; keep phase continuous from the new origin.
        resync(masterCycle);
        return mixedLevel();
    }
    if (masterCycle == time_)
        return mixedLevel();

    const uint64_t start = time_;
    int64_t        accL  = 0;
    int64_t        accR  = 0;

    while (time_ < masterCycle) {
        const uint64_t edge = nextEdgeTime();
        const uint64_t next = std::min(edge, masterCycle);

        if (next > time_) {
            const auto     level = mixedLevel();
            const int64_t  dur   = static_cast<int64_t>(next - time_);
            accL += static_cast<int64_t>(level[0]) * dur;
            accR += static_cast<int64_t>(level[1]) * dur;
            time_ = next;
        }

        if (time_ >= masterCycle)
            break;

        // One or more generators flip at `time_`.
        processEdgesAt(time_);

        // If freqInc collapsed somehow, force progress to avoid a stuck loop.
        if (nextEdgeTime() <= time_) {
            for (int ch = 0; ch < 4; ++ch) {
                if (nextEdge_[ch] <= time_)
                    nextEdge_[ch] = time_ + 1;
            }
        }
    }

    const int64_t span = static_cast<int64_t>(masterCycle - start);
    return {
        static_cast<int>(accL / span),
        static_cast<int>(accR / span),
    };
}
