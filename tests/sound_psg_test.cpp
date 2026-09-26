#include "system/sound/Sound.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <string_view>
#include <thread>
#include <vector>

struct SoundPSGTestAccess {
    static void reset(Sound &sound) {
        sound.psg_.reset();
        sound.psg_.resync(0);
    }

    static void write(Sound &sound, uint64_t masterCycle, m_byte value) {
        sound.psg_.write(masterCycle, value);
    }

    static std::array<int, 2> renderUntil(Sound &sound, double masterCycle) {
        return sound.psg_.renderUntil(masterCycle);
    }

    static uint64_t chipTime(const Sound &sound) {
        return sound.psg_.time();
    }

    static void setPanning(Sound &sound, uint8_t mask) {
        sound.psg_.setPanning(mask);
    }

    static void setPreamp(Sound &sound, int percent) {
        sound.psg_.setPreamp(percent);
    }

    static int halfWidth() {
        return Sound::PSG::kStepHalfWidth;
    }

    static int defaultPreamp() {
        return Sound::PSG::kDefaultPreamp;
    }

    // Streaming without an audio device: producers use the realtime queues
    // and renderSamples() consumes them exactly as the SDL callback does.
    static void startRealtimeWithoutDevice(Sound &sound) {
        sound.resetChipState();
        sound.realtimeMode_.store(true, std::memory_order_release);
        sound.consumerAvailable_.store(true, std::memory_order_release);
    }

    static void stopRealtimeWithoutDevice(Sound &sound) {
        sound.consumerAvailable_.store(false, std::memory_order_release);
        sound.realtimeMode_.store(false, std::memory_order_release);
    }

    static double renderCycle(const Sound &sound) {
        return sound.renderMasterCycle_;
    }

    static void renderSamples(Sound &sound, int16_t *dst, int frames) {
        sound.renderSamples(dst, frames);
    }

    static bool exerciseRealtimeQueue(Sound &sound) {
        constexpr std::size_t eventsPerProducer = 4'000;
        sound.realtimeYMEvents_.reset();

        const auto produce = [&](std::size_t producer) {
            for (std::size_t index = 0; index < eventsPerProducer; ++index) {
                Sound::TimedEvent event{};
                event.masterCycle = producer * eventsPerProducer + index;
                while (!sound.realtimeYMEvents_.tryPush(event))
                    std::this_thread::yield();
            }
        };

        std::thread first(produce, 0);
        std::thread second(produce, 1);
        first.join();
        second.join();

        std::vector<Sound::TimedEvent> observed;
        sound.realtimeYMEvents_.drainTo(observed);
        if (observed.size() != eventsPerProducer * 2 ||
            sound.realtimeYMEvents_.approximateSize() != 0)
            return false;

        std::ranges::sort(observed, {}, &Sound::TimedEvent::masterCycle);
        for (std::size_t index = 0; index < observed.size(); ++index) {
            if (observed[index].masterCycle != index)
                return false;
        }
        return true;
    }
};

namespace {

using Access = SoundPSGTestAccess;

constexpr double kMasterClock     = 53'693'175.0;
constexpr double kSampleRate      = Sound::kSampleRate;
constexpr double kCyclesPerSample = kMasterClock / kSampleRate;
constexpr int    kTickCycles      = 15 * 16;

int failures = 0;

void check(bool condition, const char *what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

struct Write {
    uint64_t masterCycle;
    m_byte   value;
};

// Drives the PSG like Sound::renderSamples(): writes are applied at their
// own master cycle before the first output sample at or after them.
std::vector<std::array<int, 2>> render(Sound &sound, std::vector<Write> writes, int frames, int preamp = 100) {
    Access::reset(sound);
    Access::setPreamp(sound, preamp);
    std::ranges::stable_sort(writes, {}, &Write::masterCycle);

    std::vector<std::array<int, 2>> out;
    out.reserve(static_cast<size_t>(frames));
    size_t next = 0;
    for (int frame = 0; frame < frames; ++frame) {
        const double time = frame * kCyclesPerSample;
        while (next < writes.size() && static_cast<double>(writes[next].masterCycle) <= time) {
            Access::write(sound, writes[next].masterCycle, writes[next].value);
            ++next;
        }
        out.push_back(Access::renderUntil(sound, time));
    }
    return out;
}

std::vector<Write> tone(int channel, int period, int attenuation) {
    return {
        {0, static_cast<m_byte>(0x80 | (channel << 5) | (period & 0x0F))},
        {0, static_cast<m_byte>((period >> 4) & 0x3F)},
        {0, static_cast<m_byte>(0x90 | (channel << 5) | attenuation)},
    };
}

std::vector<double> left(const std::vector<std::array<int, 2>> &frames) {
    std::vector<double> samples;
    samples.reserve(frames.size());
    for (const auto &frame : frames)
        samples.push_back(frame[0]);
    return samples;
}

void fft(std::vector<std::complex<double>> &data) {
    const size_t n = data.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(data[i], data[j]);
    }
    for (size_t length = 2; length <= n; length <<= 1) {
        const double              angle = -2.0 * std::numbers::pi / static_cast<double>(length);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (size_t start = 0; start < n; start += length) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < length / 2; ++k) {
                const auto even = data[start + k];
                const auto odd  = data[start + k + (length / 2)] * w;
                data[start + k]                  = even + odd;
                data[start + k + (length / 2)]   = even - odd;
                w *= step;
            }
        }
    }
}

// Ratio (dB) of the energy at the square wave's harmonics to everything else
// in 20 Hz..20 kHz: aliasing shows up as inharmonic energy.
double harmonicToAliasRatio(const std::vector<double> &samples, double fundamental) {
    constexpr size_t kSize = 32'768;
    constexpr size_t kSkip = 2'400;
    double           mean  = 0.0;
    for (size_t i = 0; i < kSize; ++i)
        mean += samples[kSkip + i];
    mean /= kSize;

    std::vector<std::complex<double>> spectrum(kSize);
    for (size_t i = 0; i < kSize; ++i) {
        // 4-term Blackman-Harris window (-92 dB sidelobes).
        const double x = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(kSize - 1);
        const double w = 0.35875 - (0.48829 * std::cos(x)) + (0.14128 * std::cos(2 * x)) - (0.01168 * std::cos(3 * x));
        spectrum[i]    = (samples[kSkip + i] - mean) * w;
    }
    fft(spectrum);

    const double binWidth = kSampleRate / kSize;
    double       harmonic = 0.0;
    double       other    = 0.0;
    for (size_t bin = 1; bin < kSize / 2; ++bin) {
        const double frequency = static_cast<double>(bin) * binWidth;
        if (frequency < 20.0 || frequency > 20'000.0)
            continue;
        const double nearest = std::round(frequency / fundamental) * fundamental;
        const double energy  = std::norm(spectrum[bin]);
        if (nearest > 0.0 && std::abs(frequency - nearest) < 6.0 * binWidth)
            harmonic += energy;
        else
            other += energy;
    }
    return 10.0 * std::log10(harmonic / std::max(other, 1e-30));
}

// Centre of an isolated step in samples[from, to): the centroid of its first
// differences sits half a sample after the step for a symmetric kernel.
double stepCentre(const std::vector<double> &samples, size_t from, size_t to) {
    double moment = 0.0;
    double total  = 0.0;
    for (size_t i = from + 1; i < to; ++i) {
        const double difference = samples[i] - samples[i - 1];
        moment += static_cast<double>(i) * difference;
        total += difference;
    }
    return (moment / total) - 0.5;
}

// Output index where a rising step crosses half of `height` (interpolated).
double halfRiseIndex(const std::vector<double> &samples, double height, size_t from = 0) {
    for (size_t i = std::max<size_t>(from, 1); i < samples.size(); ++i) {
        if (samples[i] >= height / 2.0 && samples[i - 1] < height / 2.0)
            return static_cast<double>(i - 1) + ((height / 2.0 - samples[i - 1]) / (samples[i] - samples[i - 1]));
    }
    return -1.0;
}

int risingCrossings(const std::vector<double> &samples, double level, size_t from = 0) {
    int count = 0;
    for (size_t i = std::max<size_t>(from, 1); i < samples.size(); ++i) {
        if (samples[i] >= level && samples[i - 1] < level)
            ++count;
    }
    return count;
}

uint64_t tickAtOrAfter(uint64_t masterCycle) {
    return ((masterCycle + kTickCycles - 1) / kTickCycles) * kTickCycles;
}

// Square harmonics must not fold back into the audible band. The former
// per-sample box filter only reached 19-35 dB here; GPX's blip_buf ~70 dB.
void testToneIsBandLimited(Sound &sound) {
    for (const int period : {7, 9, 20, 57, 254}) {
        const auto   samples     = left(render(sound, tone(0, period, 0), 36'000));
        const double fundamental = kMasterClock / (2.0 * kTickCycles * period);
        const double ratio       = harmonicToAliasRatio(samples, fundamental);
        char         what[96];
        std::snprintf(what, sizeof what, "period %d harmonic/alias ratio %.1f dB >= 65 dB", period, ratio);
        check(ratio >= 65.0, what);
    }
}

void testLevelsAndPanning(Sound &sound) {
    // Period 1023 holds each half-cycle for ~219 samples: the output settles
    // exactly on the channel level (2800 at full volume, preamp 100).
    auto frames = render(sound, tone(0, 1023, 0), 200);
    check(frames[150][0] == 2800 && frames[150][1] == 2800, "full-volume tone settles on 2800");

    // Attenuation follows GPX's truncated 2 dB table (-4 dB -> 1766).
    frames = render(sound, tone(1, 1023, 2), 200);
    check(frames[150][0] == 1766, "-4 dB tone settles on 1766");

    // Panning bits: left = bit (ch + 4), right = bit ch.
    Access::reset(sound);
    Access::setPreamp(sound, 100);
    Access::setPanning(sound, 0x0F);
    for (const Write &write : tone(0, 1023, 0))
        Access::write(sound, write.masterCycle, write.value);
    std::array<int, 2> sample{};
    for (int frame = 0; frame <= 150; ++frame)
        sample = Access::renderUntil(sound, frame * kCyclesPerSample);
    check(sample[0] == 0 && sample[1] == 2800, "right-only panning");

    // Default host preamp (150%) matches GPX's PSG/FM balance.
    frames = render(sound, tone(2, 1023, 0), 200, Access::defaultPreamp());
    check(frames[150][0] == 4200, "default preamp scales the level to 4200");

    // An ultrasonic tone (period 0 behaves as 1 on the ASIC) band-limits to
    // its mean, exactly half of the channel level.
    frames = render(sound, tone(0, 0, 0), 400);
    bool flat = true;
    for (int frame = 100; frame < 400; ++frame)
        flat = flat && std::abs(frames[static_cast<size_t>(frame)][0] - 1400) <= 1;
    check(flat, "period 0 tone renders as its 1400 mean");
}

void testWriteTiming(Sound &sound) {
    // The tone is high for its first ~219 samples; the volume write lands on
    // the next PSG tick and the band-limited step is centred there, delayed
    // by the kernel half width. The old renderer snapped writes to the
    // output sample grid.
    const int halfWidth = Access::halfWidth();
    for (const uint64_t writeCycle : {100'037ull, 100'080ull, 104'321ull, 110'999ull, 123'456ull}) {
        auto writes = tone(0, 1023, 15);
        writes.push_back({writeCycle, 0x90});
        const auto   samples  = left(render(sound, writes, 200));
        const double expected = (static_cast<double>(tickAtOrAfter(writeCycle)) / kCyclesPerSample) + halfWidth;
        const double observed = stepCentre(samples, 60, 150);
        char         what[128];
        std::snprintf(what, sizeof what, "write at %llu centred at %.3f (expected %.3f)",
                      static_cast<unsigned long long>(writeCycle), observed, expected);
        check(std::abs(observed - expected) < 0.02, what);
    }

    // A write stamped before the chip's current time applies at that time.
    Access::reset(sound);
    Access::setPreamp(sound, 100);
    for (const Write &write : tone(0, 1023, 15))
        Access::write(sound, write.masterCycle, write.value);
    std::vector<double> samples;
    uint64_t            chipTime = 0;
    for (int frame = 0; frame < 200; ++frame) {
        if (frame == 50) {
            chipTime = Access::chipTime(sound);
            Access::write(sound, 0, 0x90);
        }
        samples.push_back(Access::renderUntil(sound, frame * kCyclesPerSample)[0]);
    }
    const double expected = (static_cast<double>(tickAtOrAfter(chipTime)) / kCyclesPerSample) + halfWidth;
    const double observed = stepCentre(samples, 30, 110);
    char         what[128];
    std::snprintf(what, sizeof what, "late write centred at %.3f (expected %.3f)", observed, expected);
    check(chipTime > 0 && std::abs(observed - expected) < 0.02, what);
}

void testNoise(Sound &sound) {
    // Periodic noise, N/512: one 16-bit LFSR bit recirculates, so the output
    // is a pulse every 16 shifts of 2 x 16 PSG ticks: 53693175 / 122880 Hz.
    const int oneSecond = static_cast<int>(kSampleRate);
    auto      samples   = left(render(sound, {{0, 0xE0}, {0, 0xF0}}, oneSecond));
    int       pulses    = risingCrossings(samples, 1400.0);
    check(pulses == 436 || pulses == 437, "periodic noise pulses at 437 Hz");

    // Rate 3 follows tone channel 2: period 100 shifts every 2 x 100 ticks.
    samples = left(render(sound, {{0, 0xC4}, {0, 0x06}, {0, 0xE3}, {0, 0xF0}}, oneSecond));
    pulses  = risingCrossings(samples, 1400.0);
    check(pulses == 69 || pulses == 70, "periodic noise tracks tone 2 (69.9 Hz)");

    // White noise (taps 0 and 3) is high about half of the time.
    samples           = left(render(sound, {{0, 0xE4}, {0, 0xF0}}, oneSecond));
    double mean       = 0.0;
    for (const double sample : samples)
        mean += sample;
    mean /= samples.size();
    check(mean > 1300.0 && mean < 1500.0, "white noise averages half level");
    check(risingCrossings(samples, 1400.0) > 1000, "white noise toggles irregularly");
}

// Every transition's band-limited step sums to exactly its delta, so after a
// burst of activity a muted chip returns to exactly zero (no DC residue).
void testSilenceIsExact(Sound &sound) {
    std::vector<Write> writes;
    uint64_t           cycle = 0;
    for (int i = 0; i < 400; ++i) {
        cycle += 97'003;
        const int channel = i % 4;
        if (channel < 3) {
            const int period = 1 + ((i * 37) % 1023);
            writes.push_back({cycle, static_cast<m_byte>(0x80 | (channel << 5) | (period & 0x0F))});
            writes.push_back({cycle + 311, static_cast<m_byte>((period >> 4) & 0x3F)});
        } else {
            writes.push_back({cycle, static_cast<m_byte>(0xE0 | (i & 7))});
        }
        writes.push_back({cycle + 719, static_cast<m_byte>(0x90 | (channel << 5) | (i % 15))});
    }
    for (int channel = 0; channel < 4; ++channel)
        writes.push_back({cycle + 1'000'000, static_cast<m_byte>(0x9F | (channel << 5))});

    const int  frames  = static_cast<int>((cycle + 1'100'000) / kCyclesPerSample);
    const auto output  = render(sound, writes, frames, 173);
    bool       silent  = true;
    for (int frame = frames - 40; frame < frames; ++frame)
        silent = silent && output[static_cast<size_t>(frame)][0] == 0 && output[static_cast<size_t>(frame)][1] == 0;
    check(silent, "muted chip settles on exactly zero");

    // Zero-length and backwards spans are harmless.
    const auto held    = Access::renderUntil(sound, (frames - 1) * kCyclesPerSample);
    const auto snapped = Access::renderUntil(sound, 1000.0);
    check(held[0] == 0 && snapped[0] == 0, "zero and backwards spans keep the level");
}

void testHeadlessApi(Sound &sound) {
    // Before start(), mapped writes apply immediately and the diagnostic
    // render goes through the same mixer and output filters as streaming.
    sound.resetForDiagnostics();
    sound.writePSG(0x80 | 0x0E); // tone 0 period 0x0FE: 440.4 Hz
    sound.writePSG(0x0F);
    sound.writePSG(0x90);
    std::vector<int16_t> buffer(9'600 * 2);
    sound.renderForDiagnostics(buffer.data(), 9'600);
    int crossings = 0;
    for (size_t frame = 2'400; frame < 9'600; ++frame) {
        if ((buffer[(frame - 1) * 2] < 0) != (buffer[frame * 2] < 0))
            ++crossings;
    }
    // 0.15 s of 440.4 Hz: 132 zero crossings.
    check(crossings >= 130 && crossings <= 134, "headless tone at 440 Hz");
}

// While streaming, the audio callback renders the PSG next to FM on one
// timeline: a write stamped N samples ahead of the render clock is heard N
// samples in (plus the band-limiting delay), not a PSG ring later.
void testRealtimeTimeline(Sound &sound) {
    Access::startRealtimeWithoutDevice(sound);
    const double start = Access::renderCycle(sound);
    const auto   at    = [&](double frames) {
        return static_cast<uint64_t>(start + (frames * kCyclesPerSample));
    };
    // Tone 0 keeps its power-on period 0: an ultrasonic square whose mean
    // steps by half the channel level whatever the generator's phase.
    sound.writePSGAt(at(100), 0x90);

    std::vector<int16_t> buffer(512 * 2);
    Access::renderSamples(sound, buffer.data(), 512);
    Access::stopRealtimeWithoutDevice(sound);

    std::vector<double> samples;
    double              peak = 0.0;
    for (size_t frame = 0; frame < 512; ++frame) {
        samples.push_back(buffer[frame * 2]);
        peak = std::max(peak, samples.back());
    }
    const double rise = halfRiseIndex(samples, peak);
    char         what[128];
    std::snprintf(what, sizeof what, "realtime PSG write rises at %.2f (expected ~%d)", rise, 100 + Access::halfWidth());
    check(peak > 1000.0, "realtime PSG write is rendered");
    check(rise > 100.0 + Access::halfWidth() - 3.0 && rise < 100.0 + Access::halfWidth() + 4.0, what);
}

int benchmark(Sound &sound) {
    // Worst case: three period-1 tones plus noise clocked by tone 2.
    std::vector<Write> writes;
    for (int channel = 0; channel < 3; ++channel) {
        writes.push_back({0, static_cast<m_byte>(0x81 | (channel << 5))});
        writes.push_back({0, 0x00});
        writes.push_back({0, static_cast<m_byte>(0x90 | (channel << 5))});
    }
    writes.push_back({0, 0xE7});
    writes.push_back({0, 0xF0});
    constexpr int kFrames = 10 * 48'000;
    const auto    start   = std::chrono::steady_clock::now();
    const auto    output  = render(sound, writes, kFrames, Access::defaultPreamp());
    const auto    elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    std::printf("frames=%d elapsed_us=%lld last=%d\n",
                kFrames,
                static_cast<long long>(elapsed),
                output.back()[0]);
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char **argv) {
    Sound sound(nullptr);

    if (argc == 2 && std::string_view(argv[1]) == "--benchmark")
        return benchmark(sound);

    testToneIsBandLimited(sound);
    testLevelsAndPanning(sound);
    testWriteTiming(sound);
    testNoise(sound);
    testSilenceIsExact(sound);
    testHeadlessApi(sound);
    testRealtimeTimeline(sound);
    check(Access::exerciseRealtimeQueue(sound), "realtime audio queue keeps every producer event");

    if (failures != 0) {
        std::fprintf(stderr, "%d PSG check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
