#include "system/z80/Z80.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

int failures = 0;

void check(bool condition, const char *message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAILED: %s\n", message);
    ++failures;
}

// LD HL,$1000 ; loop: INC (HL) ; JR loop -- a counter at Z80 RAM $1000 that
// only moves while the core executes.
void loadCounterProgram(Z80 &z80) {
    constexpr m_byte program[] = {0x21, 0x00, 0x10, 0x34, 0x18, 0xFD};
    for (uint16_t index = 0; index < sizeof(program); ++index)
        z80.writeRAMFor68K(index, program[index]);
}

bool waitForAck(const Z80 &z80) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!z80.busRequestAcked()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

// A BUSREQ re-raised right after a release must be granted without waiting
// for the Z80 thread to wake from its pacing sleep (sound_ym2612_acquire does
// this per retry while a DAC sample plays), and the core must really be
// stopped once the ack is visible.
void testReRaisedBusRequestAcksPromptly() {
    Z80 z80(nullptr);
    z80.reset();
    loadCounterProgram(z80);
    z80.start();
    z80.setReset(false);

    constexpr int kHandshakes = 2000;
    int           immediate   = 0;
    bool          everAcked   = true;
    bool          stopped     = true;

    for (int index = 0; index < kHandshakes; ++index) {
        z80.setBusRequest(false);
        check(!z80.busRequestAcked(), "ack drops when the bus is released");
        // Let the core run for a moment so the next request can land mid-slice.
        if ((index & 15) == 0)
            std::this_thread::sleep_for(std::chrono::microseconds(300));

        z80.setBusRequest(true);
        if (z80.busRequestAcked())
            ++immediate;
        everAcked = waitForAck(z80) && everAcked;

        const m_byte before = z80.readRAMFor68K(0x1000);
        std::this_thread::yield();
        stopped = (z80.readRAMFor68K(0x1000) == before) && stopped;
    }

    check(everAcked, "every BUSREQ is eventually acknowledged");
    check(stopped, "the core does not execute while the bus is acknowledged");
    check(immediate * 2 >= kHandshakes, "most re-raised BUSREQs are acknowledged without a thread round trip");
    std::printf("immediate acks: %d / %d\n", immediate, kHandshakes);

    z80.setBusRequest(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const m_byte first = z80.readRAMFor68K(0x1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(z80.readRAMFor68K(0x1000) != first, "the core runs again after the bus is released");

    z80.stop();
}

} // namespace

int main() {
    testReRaisedBusRequestAcksPromptly();

    if (failures != 0) {
        std::fprintf(stderr, "%d Z80 bus check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
