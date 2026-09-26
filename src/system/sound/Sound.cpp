#include "Sound.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include "Logger.hpp"

namespace {

constexpr uint32_t kYM2612Clock   = 53'693'175u / 7u;
constexpr uint64_t kMasterClockHz = 53'693'175ull;
constexpr double   kMasterClock   = 53'693'175.0;
// A small scheduling margin keeps producer events in the renderer's future.
// SDL and the physical device add their own buffering, so a larger software
// margin makes music and effects perceptibly trail the game. Inaccurate late
// events are preferable to making gameplay/audio response feel sluggish.
constexpr double   kEventLatencyCycles = 0.016 * kMasterClock; // ~16 ms (was 12)
constexpr double   kSnapCycles         = 0.250 * kMasterClock; // resync hard beyond this drift
constexpr double   kMaxRateTrim        = 0.005;                // ±0.5% render-rate trim toward the latency target
constexpr int      kRenderChunkFrames  = 256;
constexpr int      kRingBufferFrames   = 4096;
/// Soft prebuffer for the mixed output ring (~21 ms at 48 kHz). The callback
/// used to pull just-in-time only; a modest watermark absorbs host scheduling
/// jitter without making audio feel laggy.
constexpr int      kRingTargetFrames = 1024;
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

// 2 dB steps from full scale: Genesis Plus GX's PSG_MAX_VOLUME (2800) times
// 10^(-step/10), truncated to integers exactly as GPX's uint16 table does.
// With the ~1.5x host preamp this balances against ymfm like VA4 MD1 hardware.
int psgVolume(uint8_t attenuation) {
    static constexpr std::array<int, 16> kVolume = {
        2800, //  MAX
        2224, // -2 dB
        1766, // -4 dB
        1403, // -6 dB
        1114, // -8 dB
        885,  // -10 dB
        703,  // -12 dB
        558,  // -14 dB
        443,  // -16 dB
        352,  // -18 dB
        280,  // -20 dB
        222,  // -22 dB
        176,  // -24 dB
        140,  // -26 dB
        111,  // -28 dB
        0,    // OFF
    };
    return kVolume[attenuation & 0x0F];
}

// White-noise XOR of the tapped bits (mask selects which LFSR bits feed back).
// For the integrated ASIC, mask is 0x9 → bits 0 and 3.
int noiseFeedbackBit(int shiftValue, int bitMask) {
    return std::popcount(static_cast<unsigned>(shiftValue & bitMask)) & 1;
}

// ── Band-limited step synthesis ──────────────────────────────────────────────
//
// The PSG is a set of square/noise generators clocked at ~3.58 MHz / 16. Point
// sampling (or averaging over one output period) folds their harmonics back
// into the audible band as inharmonic tones. Like GPX's blip_buf, each output
// transition is instead rendered as a band-limited step: the difference of a
// windowed-sinc step response sampled at the host rate. Integrating those
// differences yields an alias-free output that settles on the exact level.

constexpr int kKernelHalfWidth = 16; // == Sound::PSG::kStepHalfWidth
constexpr int kStepTaps        = 2 * kKernelHalfWidth;
constexpr int kStepPhaseBits   = 6; // sub-sample phases stored in the table
constexpr int kStepPhases      = 1 << kStepPhaseBits;
constexpr int kStepInterpBits  = 10; // linear interpolation between phases
constexpr int kStepKernelBits  = 20; // each table row sums to 1 << this
/// Output scale of an interpolated kernel: every transition contributes
/// exactly delta << kStepShift once all of its taps have been emitted.
constexpr int kStepShift = kStepKernelBits + kStepInterpBits;
/// Kaiser-windowed sinc: cutoff 0.47 fs, beta 10 (about 100 dB stopband).
constexpr double kStepCutoff = 0.47;
constexpr double kStepBeta   = 10.0;

double besselI0(double x) {
    double sum  = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64; ++k) {
        const double ratio = x / (2.0 * k);
        term *= ratio * ratio;
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

/// Band-limited impulse response at `u` host samples from the transition.
double stepImpulse(double u) {
    const double halfWidth = static_cast<double>(kKernelHalfWidth);
    if (std::abs(u) >= halfWidth)
        return 0.0;
    const double x      = 2.0 * kStepCutoff * u;
    const double sinc   = (x == 0.0) ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
    const double r      = u / halfWidth;
    const double window = besselI0(kStepBeta * std::sqrt(1.0 - (r * r))) / besselI0(kStepBeta);
    return 2.0 * kStepCutoff * sinc * window;
}

using StepKernel = std::array<std::array<double, kStepTaps>, kStepPhases + 1>;

/// Row p holds the per-sample differences of a unit step placed p/kStepPhases
/// of a sample after the first tap's centre: tap k of row p is the impulse's
/// area over [k - W - p/P, k - W + 1 - p/P]. Taps are integers and rows sum to
/// exactly 1 << kStepKernelBits, so a fully emitted transition leaves no DC
/// error; they are stored as doubles because every product formed from them
/// stays an exact integer and the accumulation vectorises.
StepKernel buildStepKernel() {
    // Running integral of the impulse on a 1/kStepPhases grid spanning the
    // window (Simpson's rule per cell; far below integer resolution).
    constexpr int kCells = kStepTaps * kStepPhases;
    std::array<double, kCells + 1> integral{};
    const double cell = 1.0 / kStepPhases;
    for (int i = 0; i < kCells; ++i) {
        const double left = (static_cast<double>(i) * cell) - kKernelHalfWidth;
        const double area =
            (stepImpulse(left) + (4.0 * stepImpulse(left + (cell / 2.0))) + stepImpulse(left + cell)) * cell / 6.0;
        integral[static_cast<size_t>(i) + 1] = integral[static_cast<size_t>(i)] + area;
    }
    const double total = integral.back();

    StepKernel kernel{};
    for (int phase = 0; phase <= kStepPhases; ++phase) {
        std::array<int64_t, kStepTaps> row{};
        int64_t                        rowSum  = 0;
        size_t                         largest = 0;
        for (int tap = 0; tap < kStepTaps; ++tap) {
            // Grid index of the tap's lower bound (tap - W - p/P); the row for
            // phase P equals row 0 shifted by one tap.
            const int    lower = (tap * kStepPhases) - phase;
            const double below = (lower <= 0) ? 0.0 : integral[static_cast<size_t>(lower)];
            const double above =
                (lower + kStepPhases <= 0) ? 0.0 : integral[static_cast<size_t>(lower + kStepPhases)];
            row[static_cast<size_t>(tap)] =
                std::llround((above - below) / total * static_cast<double>(1 << kStepKernelBits));
            rowSum += row[static_cast<size_t>(tap)];
            if (std::abs(row[static_cast<size_t>(tap)]) > std::abs(row[largest]))
                largest = static_cast<size_t>(tap);
        }
        row[largest] += (int64_t{1} << kStepKernelBits) - rowSum;
        for (size_t tap = 0; tap < row.size(); ++tap)
            kernel[static_cast<size_t>(phase)][tap] = static_cast<double>(row[tap]);
    }
    return kernel;
}

const StepKernel &stepKernel() {
    static const StepKernel kernel = buildStepKernel();
    return kernel;
}

} // namespace

Sound::RealtimeEventQueue::RealtimeEventQueue()
    : slots_(std::make_unique<Slot[]>(kEventQueueCapacity)) {
    reset();
}

bool Sound::RealtimeEventQueue::tryPush(const TimedEvent &event) {
    std::size_t position = enqueuePosition_.load(std::memory_order_relaxed);
    for (;;) {
        Slot &slot = slots_[position % kEventQueueCapacity];
        const std::size_t sequence = slot.sequence.load(std::memory_order_acquire);
        const auto difference = static_cast<std::intptr_t>(sequence)
                              - static_cast<std::intptr_t>(position);
        if (difference == 0) {
            if (enqueuePosition_.compare_exchange_weak(position,
                                                       position + 1,
                                                       std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
                slot.event = event;
                slot.sequence.store(position + 1, std::memory_order_release);
                return true;
            }
        } else if (difference < 0) {
            return false;
        } else {
            position = enqueuePosition_.load(std::memory_order_relaxed);
        }
    }
}

void Sound::RealtimeEventQueue::drainTo(std::vector<TimedEvent> &destination) {
    while (destination.size() < kEventQueueCapacity) {
        Slot &slot = slots_[dequeuePosition_ % kEventQueueCapacity];
        const std::size_t sequence = slot.sequence.load(std::memory_order_acquire);
        const auto difference = static_cast<std::intptr_t>(sequence)
                              - static_cast<std::intptr_t>(dequeuePosition_ + 1);
        if (difference != 0)
            break;
        destination.push_back(slot.event);
        slot.sequence.store(dequeuePosition_ + kEventQueueCapacity, std::memory_order_release);
        ++dequeuePosition_;
    }
    publishedDequeuePosition_.store(dequeuePosition_, std::memory_order_release);
}

void Sound::RealtimeEventQueue::reset() {
    enqueuePosition_.store(0, std::memory_order_relaxed);
    dequeuePosition_ = 0;
    publishedDequeuePosition_.store(0, std::memory_order_relaxed);
    for (std::size_t index = 0; index < kEventQueueCapacity; ++index)
        slots_[index].sequence.store(index, std::memory_order_relaxed);
}

std::size_t Sound::RealtimeEventQueue::approximateSize() const {
    const std::size_t enqueued = enqueuePosition_.load(std::memory_order_relaxed);
    const std::size_t dequeued = publishedDequeuePosition_.load(std::memory_order_relaxed);
    return std::min(enqueued - dequeued, kEventQueueCapacity);
}

Sound::Sound(MegaDriveEnvironment *env)
    : env_(env), mutex_(SDL_CreateMutex()), ym_(ymInterface_) {
    baseTimeNS_   = SDL_GetTicksNS();
    fmSampleRate_ = ym_.sample_rate(kYM2612Clock);
    pendingYMEvents_.reserve(kEventQueueCapacity);
    pendingPSGEvents_.reserve(kEventQueueCapacity);
    renderEvents_.reserve(kEventQueueCapacity * 2);
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
    resetChipState();
    SDL_UnlockMutex(mutex_);

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        Logger::log("Sound: could not initialize SDL audio: %s", SDL_GetError());
        return;
    }
    audioInitialized_ = true;

    SDL_AudioSpec spec{};
    spec.format   = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq     = kSampleRate;

    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audioCallback, this);
    if (!stream_) {
        Logger::log("Sound: could not open SDL audio stream: %s", SDL_GetError());
        return;
    }

    // Prefill the mixed ring so the first device pulls do not underrun into
    // silence (crackling on start).
    ensureRingFrames(kRingTargetFrames);

    consumerAvailable_.store(true, std::memory_order_release);
    SDL_ResumeAudioStreamDevice(stream_);
}

void Sound::stop() {
    consumerAvailable_.store(false, std::memory_order_release);
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    if (audioInitialized_) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audioInitialized_ = false;
    }
    realtimeMode_.store(false, std::memory_order_release);
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
    processEventsUntil(masterCycles);
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
        prepareYMEvent(event);
        if (!realtimeYMEvents_.tryPush(event))
            queueFullDropCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    SDL_LockMutex(mutex_);
    if (enqueueYMEvent(event))
        processEventsUntil(masterCycles);
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
        // The audio callback renders the PSG next to FM on the same timeline.
        if (!consumerAvailable_.load(std::memory_order_acquire)) {
            unavailableDropCount_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        preparePSGEvent(event);
        if (!realtimePSGEvents_.tryPush(event))
            queueFullDropCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    SDL_LockMutex(mutex_);
    if (enqueuePSGEvent(event))
        processEventsUntil(masterCycles);
    SDL_UnlockMutex(mutex_);
}

void Sound::resetForDiagnostics() {
    SDL_LockMutex(mutex_);
    resetChipState();
    SDL_UnlockMutex(mutex_);
}

void Sound::renderForDiagnostics(int16_t *dst, int frames) {
    renderSamples(dst, frames);
}

Sound::Diagnostics Sound::diagnostics() const {
    const uint64_t contentionDrops  = contentionDropCount_.load(std::memory_order_relaxed);
    const uint64_t queueFullDrops   = queueFullDropCount_.load(std::memory_order_relaxed);
    const uint64_t unavailableDrops = unavailableDropCount_.load(std::memory_order_relaxed);
    return Diagnostics{
        .audioFramesRendered   = audioFramesRendered_.load(std::memory_order_relaxed),
        .underruns             = underrunCount_.load(std::memory_order_relaxed),
        .overruns              = overrunCount_.load(std::memory_order_relaxed),
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
        .psgRingBufferedFrames = 0,
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
    psg_.resync(static_cast<uint64_t>(renderMasterCycle_));
    lastRenderedMasterCycle_.store(0, std::memory_order_relaxed);
    queuedYMAddress_.store(0, std::memory_order_relaxed);
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
    realtimeYMEvents_.reset();
    realtimePSGEvents_.reset();
    renderEvents_.clear();
    ymQueuedEventCount_.store(0, std::memory_order_relaxed);
    psgQueuedEventCount_.store(0, std::memory_order_relaxed);
    queuedEventCount_.store(0, std::memory_order_relaxed);
    ringBuffer_.assign(kRingBufferFrames * 2, 0);
    ringReadFrame_      = 0;
    ringWriteFrame_     = 0;
    ringBufferedFrames_ = 0;
    ringBufferedFramesSnapshot_.store(0, std::memory_order_relaxed);
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

    // Always top up to the soft watermark so a single late callback does not
    // drain the ring to empty (underrun → crackle). Still honour larger
    // device pulls when the OS asks for more than the watermark.
    const size_t want =
        std::max(static_cast<size_t>(frames), static_cast<size_t>(kRingTargetFrames));
    const size_t capacityFrames = ringBuffer_.size() / 2;
    while (ringBufferedFrames_ < want) {
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
    for (int base = 0; base < frames; base += kRenderChunkFrames) {
        const int chunkFrames = std::min(kRenderChunkFrames, frames - base);

        // Keep the render clock kEventLatencyCycles behind the producers'
        // wall clock: snap on gross drift (startup, host stalls), otherwise
        // trim the per-sample step so the pitch shift stays inaudible.
        const double target = static_cast<double>(masterCyclesNow()) - kEventLatencyCycles;
        double       error  = renderMasterCycle_ - target;
        if (std::abs(error) > kSnapCycles) {
            renderMasterCycle_ = std::max(target, 0.0);
            psg_.resync(static_cast<uint64_t>(renderMasterCycle_));
            error = 0.0;
        }
        const double step = (kMasterClock / static_cast<double>(kSampleRate)) *
                            (1.0 - std::clamp(error / kSnapCycles, -kMaxRateTrim, kMaxRateTrim));

        // Drain YM and PSG events for this chunk. Both chips render below on
        // one timeline, so a PSG write lands in the same output sample as a YM
        // write stamped with the same master cycle.
        renderEvents_.clear();
        const uint64_t chunkLastCycle =
            static_cast<uint64_t>(renderMasterCycle_ + (step * static_cast<double>(chunkFrames - 1)));
        const bool realtime = realtimeMode_.load(std::memory_order_acquire);
        if (realtime) {
            realtimeYMEvents_.drainTo(pendingYMEvents_);
            realtimePSGEvents_.drainTo(pendingPSGEvents_);
        } else {
            SDL_LockMutex(mutex_);
        }
        drainQueueUntil(pendingYMEvents_, chunkLastCycle, renderEvents_);
        drainQueueUntil(pendingPSGEvents_, chunkLastCycle, renderEvents_);
        setYMQueuedCount(pendingYMEvents_.size());
        setPSGQueuedCount(pendingPSGEvents_.size());
        if (!realtime)
            SDL_UnlockMutex(mutex_);
        std::stable_sort(renderEvents_.begin(), renderEvents_.end(), [](const TimedEvent &a, const TimedEvent &b) {
            return a.masterCycle < b.masterCycle;
        });

        size_t nextEvent = 0;
        for (int i = 0; i < chunkFrames; ++i) {
            const int      frame       = base + i;
            const uint64_t masterCycle = static_cast<uint64_t>(renderMasterCycle_);
            while (nextEvent < renderEvents_.size() && renderEvents_[nextEvent].masterCycle <= masterCycle) {
                const TimedEvent &event = renderEvents_[nextEvent++];
                if (event.type == EventType::YMWrite)
                    applyYMEvent(event);
                else
                    applyPSGEvent(event, false);
            }
            ymInterface_.syncTimersToMasterCycle(masterCycle);
            const auto fm  = renderFM();
            const auto psg = psg_.renderUntil(renderMasterCycle_);

            const auto filtered = filterOutput({fm[0] + psg[0], fm[1] + psg[1]});
            dst[frame * 2 + 0]  = clampMixedSample(filtered[0], 0);
            dst[frame * 2 + 1]  = clampMixedSample(filtered[1], 1);
            lastRenderedMasterCycle_.store(masterCycle, std::memory_order_relaxed);
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
    if (pendingYMEvents_.size() >= kEventQueueCapacity) {
        queueFullDropCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    prepareYMEvent(event);
    pendingYMEvents_.push_back(event);
    setYMQueuedCount(pendingYMEvents_.size());
    return true;
}

void Sound::prepareYMEvent(TimedEvent &event) {
    const uint64_t lastRendered = lastRenderedMasterCycle_.load(std::memory_order_relaxed);
    if (event.masterCycle < lastRendered) {
        event.masterCycle = lastRendered;
        lateEventCount_.fetch_add(1, std::memory_order_relaxed);
    }

    if ((event.port & 1u) == 0) {
        if (event.port == 0)
            queuedYMAddress_.store(event.value, std::memory_order_relaxed);
    } else {
        const uint8_t address = queuedYMAddress_.load(std::memory_order_relaxed);
        if (event.port != 1 || address < 0x24 || address > 0x27)
            return;
        // Optimistically clear the timer flags in the published status so
        // a driver that just wrote $27 to reset them doesn't re-read the
        // stale set flags before the renderer applies the write.
        event.timerRegister = true;
        cachedStatus_.fetch_and(static_cast<uint8_t>(~0x03u), std::memory_order_relaxed);
    }
}

bool Sound::enqueuePSGEvent(TimedEvent event) {
    if (pendingPSGEvents_.size() >= kEventQueueCapacity) {
        queueFullDropCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    preparePSGEvent(event);
    pendingPSGEvents_.push_back(event);
    setPSGQueuedCount(pendingPSGEvents_.size());
    return true;
}

void Sound::preparePSGEvent(TimedEvent &event) {
    const uint64_t lastRendered = lastRenderedMasterCycle_.load(std::memory_order_relaxed);
    if (event.masterCycle < lastRendered) {
        event.masterCycle = lastRendered;
        lateEventCount_.fetch_add(1, std::memory_order_relaxed);
    }
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
    // Headless path: caller holds mutex_. Apply YM + PSG in timestamp order.
    // Nothing renders audio in step with these writes, so PSG writes land at
    // the chip's current time instead of advancing it to the wall clock.
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
            applyPSGEvent(event, true);
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

void Sound::applyPSGEvent(const TimedEvent &event, bool immediate) {
    if (FILE *log = ymLogFile()) {
        std::fprintf(log,
                     "P %llu port=%u val=%02X\n",
                     static_cast<unsigned long long>(event.masterCycle),
                     event.port,
                     event.value);
    }
    psg_.write(immediate ? psg_.time() : event.masterCycle, event.value);
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
    static_assert(kStepHalfWidth == kKernelHalfWidth);
    (void)stepKernel(); // build the table here, not on the audio thread

    // Mega Drive uses the VDP-integrated ASIC clone (not the discrete SN76489).
    zeroFreqInc_     = kTickCycles; // period 0 ≡ 1
    noiseShiftWidth_ = 15;
    noiseBitMask_    = 0x9;
    preamp_          = kDefaultPreamp;
    panMask_         = 0xFF;
    clock_           = 0;
    tickOrigin_      = 0;
    sampleTime_      = 0.0;
    latch_           = 3; // tone #2 attenuation latched on power-on (315-5313A)

    for (int i = 0; i < 4; ++i) {
        regs_[i * 2]     = 0;
        regs_[i * 2 + 1] = 0;
        freqInc_[i]      = (i < 3) ? zeroFreqInc_ : (0x10 * kTickCycles);
        nextEdge_[i]     = 0;
        polarity_[i]     = -1;
        chanOutL_[i]     = 0;
        chanOutR_[i]     = 0;
    }
    noiseShift_ = 1 << noiseShiftWidth_;

    // Every channel is silent, so the band-limited output restarts at zero.
    transitions_.clear();
    transitions_.reserve(kMaxTransitions);
    stepSum_.fill(0.0);
    stepDifference_.fill(0.0);
    levelSum_        = 0.0;
    levelDifference_ = 0.0;
    stepRead_        = 0;
    setPanning(panMask_);
}

void Sound::PSG::resync(uint64_t masterCycle) {
    clock_      = masterCycle;
    tickOrigin_ = masterCycle;
    sampleTime_ = static_cast<double>(masterCycle);
    // Schedule the next polarity flip at the new origin (same as GPX leaving
    // freqCounter at the frame boundary). No silent half-period of lag.
    for (int i = 0; i < 4; ++i)
        nextEdge_[i] = masterCycle;
    // Transitions from the old timeline still belong to the output level;
    // land them on the first sample of the new one.
    for (Transition &transition : transitions_)
        transition.masterCycle = masterCycle;
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
        updateChannelOut(ch);
    }
}

bool Sound::PSG::channelHigh(int channel) const {
    return (channel < 3) ? (polarity_[channel] > 0) : ((noiseShift_ & 1) != 0);
}

// Recompute a channel's stereo output from its volume register; a channel
// whose output is currently high steps to the new level at the chip's time.
void Sound::PSG::updateChannelOut(int channel) {
    const int volume = regs_[channel * 2 + 1];
    const int left   = (volume * chanAmpL_[channel]) / 100;
    const int right  = (volume * chanAmpR_[channel]) / 100;
    if (channelHigh(channel))
        addTransition(clock_, left - chanOutL_[channel], right - chanOutR_[channel]);
    chanOutL_[channel] = left;
    chanOutR_[channel] = right;
}

void Sound::PSG::updateToneFreq(int channel, int period) {
    regs_[channel * 2] = period & 0x3FF;
    // The next edge keeps its schedule; only later half-periods change.
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

uint64_t Sound::PSG::tickAtOrAfter(uint64_t masterCycle) const {
    if (masterCycle <= tickOrigin_)
        return tickOrigin_;
    const uint64_t ticks = (masterCycle - tickOrigin_ + kTickCycles - 1) / kTickCycles;
    return tickOrigin_ + (ticks * kTickCycles);
}

void Sound::PSG::addTransition(uint64_t masterCycle, int left, int right) {
    if ((left | right) == 0)
        return;
    if (transitions_.size() >= kMaxTransitions) {
        // Nothing has rendered for a long time (headless writes): fold the
        // backlog into one step so the queue stays bounded and the level exact.
        Transition merged{.masterCycle = transitions_.front().masterCycle};
        for (const Transition &transition : transitions_) {
            merged.masterCycle = std::min(merged.masterCycle, transition.masterCycle);
            merged.left += transition.left;
            merged.right += transition.right;
        }
        transitions_.clear();
        transitions_.push_back(merged);
    }
    transitions_.push_back({.masterCycle = masterCycle, .left = left, .right = right});
}

void Sound::PSG::runTone(int channel, uint64_t masterCycle) {
    uint64_t edge = nextEdge_[channel];
    if (edge >= masterCycle)
        return;

    const uint64_t period = static_cast<uint64_t>(freqInc_[channel]);
    int            state  = polarity_[channel];
    const int      left   = chanOutL_[channel];
    const int      right  = chanOutR_[channel];
    if ((left | right) == 0) {
        // Muted channels still advance phase, but emit no transitions.
        const uint64_t flips = ((masterCycle - edge - 1) / period) + 1;
        if (flips & 1u)
            state = -state;
        edge += flips * period;
    } else {
        do {
            state = -state;
            addTransition(edge, state * left, state * right);
            edge += period;
        } while (edge < masterCycle);
    }
    polarity_[channel] = state;
    nextEdge_[channel] = edge;
}

void Sound::PSG::runNoise(uint64_t masterCycle) {
    uint64_t       edge   = nextEdge_[3];
    const uint64_t period = static_cast<uint64_t>(freqInc_[3]);
    int            state  = polarity_[3];
    int            shift  = noiseShift_;
    const bool     white  = (regs_[6] & 0x04) != 0;

    while (edge < masterCycle) {
        state = -state;
        // The LFSR advances only on the rising edge of the noise clock.
        if (state > 0) {
            const int output   = shift & 1;
            const int feedback = white ? noiseFeedbackBit(shift, noiseBitMask_) : output;
            shift              = (shift >> 1) | (feedback << noiseShiftWidth_);
            const int change   = (shift & 1) - output;
            if (change != 0)
                addTransition(edge, change * chanOutL_[3], change * chanOutR_[3]);
        }
        edge += period;
    }

    polarity_[3] = state;
    noiseShift_  = shift;
    nextEdge_[3] = edge;
}

void Sound::PSG::runUntil(uint64_t masterCycle) {
    if (masterCycle <= clock_)
        return;
    for (int channel = 0; channel < 3; ++channel)
        runTone(channel, masterCycle);
    runNoise(masterCycle);
    clock_ = masterCycle;
}

void Sound::PSG::write(uint64_t masterCycle, m_byte value) {
    // Like GPX's psg_write(): run the generators up to the write, which then
    // takes effect on the first PSG clock tick at or after it.
    runUntil(tickAtOrAfter(std::max(masterCycle, clock_)));

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
            if (noiseShift_ & 1)
                addTransition(clock_, -chanOutL_[3], -chanOutR_[3]);
            noiseShift_ = 1 << noiseShiftWidth_;
            break;
        }
        default:
            // Attenuation registers 1, 3, 5 (tones) and 7 (noise).
            regs_[index] = psgVolume(static_cast<uint8_t>(value & 0x0F));
            updateChannelOut(index >> 1);
            break;
    }
}

void Sound::PSG::addStep(double fraction, int left, int right) {
    const auto &kernel = stepKernel();
    const int   position =
        static_cast<int>(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(1 << (kStepPhaseBits + kStepInterpBits)));
    const int   phase  = std::min(position >> kStepInterpBits, kStepPhases - 1);
    const int   weight = position - (phase << kStepInterpBits);
    const auto &before = kernel[static_cast<size_t>(phase)];
    const auto &after  = kernel[static_cast<size_t>(phase + 1)];

    // A transition inside the interval ending at output sample n touches
    // samples n - kStepHalfWidth .. n + kStepHalfWidth - 1, which start at
    // stepRead_. The phase interpolation weights fold into the delta.
    const auto accumulate = [&](std::array<double, kStepBufferSize> &buffer, double delta) {
        const double beforeWeight = delta * static_cast<double>((1 << kStepInterpBits) - weight);
        const double afterWeight  = delta * static_cast<double>(weight);
        double      *slot         = buffer.data() + stepRead_;
        for (size_t tap = 0; tap < static_cast<size_t>(kStepTaps); ++tap)
            slot[tap] += (before[tap] * beforeWeight) + (after[tap] * afterWeight);
    };
    accumulate(stepSum_, static_cast<double>(left) + right);
    if (left != right)
        accumulate(stepDifference_, static_cast<double>(left) - right);
}

void Sound::PSG::flushTransitions(double masterCycle) {
    const double span = masterCycle - sampleTime_;
    size_t       keep = 0;
    for (const Transition &transition : transitions_) {
        const double at = static_cast<double>(transition.masterCycle);
        if (at >= masterCycle) {
            // Scheduled past this sample (a write rounded up to the next tick).
            transitions_[keep++] = transition;
            continue;
        }
        addStep(span > 0.0 ? (at - sampleTime_) / span : 0.0, transition.left, transition.right);
    }
    transitions_.resize(keep);
}

std::array<int, 2> Sound::PSG::emitSample() {
    levelSum_ += stepSum_[stepRead_];
    levelDifference_ += stepDifference_[stepRead_];
    stepSum_[stepRead_]        = 0.0;
    stepDifference_[stepRead_] = 0.0;
    if (++stepRead_ == kStepBlock) {
        // Move the live window back to the front; everything before
        // stepRead_ has been emitted and cleared already.
        for (auto *buffer : {&stepSum_, &stepDifference_}) {
            std::copy_n(buffer->begin() + kStepBlock, 2 * kStepHalfWidth, buffer->begin());
            std::fill(buffer->begin() + kStepBlock, buffer->end(), 0.0);
        }
        stepRead_ = 0;
    }

    // (sum ± difference) is twice the channel level; round to nearest.
    constexpr int     kShift = kStepShift + 1;
    constexpr int64_t kRound = int64_t{1} << (kShift - 1);
    return {
        static_cast<int>((static_cast<int64_t>(levelSum_ + levelDifference_) + kRound) >> kShift),
        static_cast<int>((static_cast<int64_t>(levelSum_ - levelDifference_) + kRound) >> kShift),
    };
}

std::array<int, 2> Sound::PSG::renderUntil(double masterCycle) {
    if (masterCycle < sampleTime_) {
        // Host timeline snapped backwards; keep phase continuous from the new origin.
        resync(static_cast<uint64_t>(std::max(masterCycle, 0.0)));
    }

    // Generator edges strictly before this sample belong to it.
    runUntil(static_cast<uint64_t>(std::ceil(masterCycle)));
    flushTransitions(masterCycle);
    sampleTime_ = masterCycle;
    return emitSample();
}
