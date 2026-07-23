#include "system/sound/Sound.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>

struct SoundPSGTestAccess {
    static void reset(Sound &sound) {
        sound.psg_.reset();
        sound.psg_.resync(0);
    }

    static void write(Sound &sound, m_byte value) {
        sound.psg_.write(value);
    }

    static std::array<int, 2> renderUntil(Sound &sound, uint64_t masterCycle) {
        return sound.psg_.renderUntil(masterCycle);
    }

    static void setPanning(Sound &sound, uint8_t mask) {
        sound.psg_.setPanning(mask);
    }

    static void setPreamp(Sound &sound, int percent) {
        sound.psg_.setPreamp(percent);
    }

    static void resetAll(Sound &sound) {
        sound.resetChipState();
    }

    static void renderChunk(Sound &sound, int frames) {
        sound.renderPsgChunk(frames);
    }

    static size_t ringBuffered(const Sound &sound) {
        return sound.psgRingBuffered_.load(std::memory_order_relaxed);
    }

    static size_t ringWriteFrame(const Sound &sound) {
        return sound.psgRingWriteFrame_;
    }

    static uint64_t overruns(const Sound &sound) {
        return sound.psgOverrunCount_.load(std::memory_order_relaxed);
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

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime  = 1099511628211ull;

void hashInt(uint64_t &hash, int value) {
    const uint32_t bits = static_cast<uint32_t>(value);
    for (int shift = 0; shift < 32; shift += 8) {
        hash ^= (bits >> shift) & 0xFFu;
        hash *= kFnvPrime;
    }
}

uint64_t renderSequence(Sound &sound, int frames) {
    SoundPSGTestAccess::reset(sound);
    SoundPSGTestAccess::setPreamp(sound, 173);
    SoundPSGTestAccess::setPanning(sound, 0xDB);

    // Three independently clocked tones plus fast white noise.
    SoundPSGTestAccess::write(sound, 0x80 | 0x07);
    SoundPSGTestAccess::write(sound, 0x02);
    SoundPSGTestAccess::write(sound, 0x90 | 0x01);
    SoundPSGTestAccess::write(sound, 0xA0 | 0x03);
    SoundPSGTestAccess::write(sound, 0x11);
    SoundPSGTestAccess::write(sound, 0xB0 | 0x04);
    SoundPSGTestAccess::write(sound, 0xC0 | 0x01);
    SoundPSGTestAccess::write(sound, 0x00);
    SoundPSGTestAccess::write(sound, 0xD0 | 0x07);
    SoundPSGTestAccess::write(sound, 0xE0 | 0x04);
    SoundPSGTestAccess::write(sound, 0xF0 | 0x03);

    uint64_t hash  = kFnvOffset;
    uint64_t cycle = 0;
    for (int frame = 0; frame < frames; ++frame) {
        cycle += 1u + ((static_cast<uint64_t>(frame) * 977u) % 2237u);

        // Exercise writes on active oscillators and all noise clock modes.
        if ((frame % 997) == 113)
            SoundPSGTestAccess::write(sound, static_cast<m_byte>(0x20u | ((frame / 997) & 0x3F)));
        if ((frame % 1291) == 277)
            SoundPSGTestAccess::write(sound, static_cast<m_byte>(0xE0u | ((frame / 1291) & 0x07)));
        if ((frame % 1877) == 419)
            SoundPSGTestAccess::write(sound, static_cast<m_byte>(0x90u | ((frame / 1877) & 0x0F)));

        const auto sample = SoundPSGTestAccess::renderUntil(sound, cycle);
        hashInt(hash, sample[0]);
        hashInt(hash, sample[1]);
    }

    // Exercise zero-span and backwards timeline handling as well.
    const auto held = SoundPSGTestAccess::renderUntil(sound, cycle);
    hashInt(hash, held[0]);
    hashInt(hash, held[1]);
    const auto snapped = SoundPSGTestAccess::renderUntil(sound, cycle / 2);
    hashInt(hash, snapped[0]);
    hashInt(hash, snapped[1]);
    return hash;
}

} // namespace

int main(int argc, char **argv) {
    Sound sound(nullptr);

    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
        constexpr int kFrames = 2'000'000;
        const auto    start   = std::chrono::steady_clock::now();
        const uint64_t hash   = renderSequence(sound, kFrames);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        std::printf("frames=%d elapsed_us=%lld hash=%016llx\n",
                    kFrames,
                    static_cast<long long>(elapsed),
                    static_cast<unsigned long long>(hash));
        return 0;
    }

    constexpr uint64_t kExpectedHash = 0xF27F92F7291ED511ull;
    const uint64_t     actual        = renderSequence(sound, 50'000);
    if (actual != kExpectedHash) {
        std::fprintf(stderr,
                     "PSG regression hash mismatch: expected=%016llx actual=%016llx\n",
                     static_cast<unsigned long long>(kExpectedHash),
                     static_cast<unsigned long long>(actual));
        return EXIT_FAILURE;
    }

    SoundPSGTestAccess::resetAll(sound);
    SoundPSGTestAccess::renderChunk(sound, 256);
    if (SoundPSGTestAccess::ringBuffered(sound) != 256 ||
        SoundPSGTestAccess::ringWriteFrame(sound) != 256 ||
        SoundPSGTestAccess::overruns(sound) != 0) {
        std::fputs("PSG chunk did not publish its frames as one batch\n", stderr);
        return EXIT_FAILURE;
    }

    SoundPSGTestAccess::renderChunk(sound, 4'000);
    if (SoundPSGTestAccess::ringBuffered(sound) != 4'096 ||
        SoundPSGTestAccess::ringWriteFrame(sound) != 0 ||
        SoundPSGTestAccess::overruns(sound) != 160) {
        std::fputs("PSG chunk did not clamp correctly at the ring boundary\n", stderr);
        return EXIT_FAILURE;
    }
    if (!SoundPSGTestAccess::exerciseRealtimeQueue(sound)) {
        std::fputs("realtime audio queue lost or duplicated producer events\n", stderr);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
