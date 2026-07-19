/**
 * @file TestVDPTest01.cpp
 * @brief Test 01 — register read/write and status register.
 */

#include "TestVDP.hpp"
#include <cstdio>
#include "Logger.hpp"

void VDPTester::testRegisters() {
    Logger::log("\n[TEST 01] registers\n");
    initVDP();

    uint16_t status = vdp().readControlPort();
    uint16_t hv     = vdp().readHVCounter();

    Logger::log("  status register = 0x%04X\n", status);
    Logger::log("  HV counter      = 0x%04X  (V=%u, H=%u)\n",
           hv,
           static_cast<unsigned>((hv >> 8) & 0xFF),
           static_cast<unsigned>(hv & 0xFF));

    bool ok = (status & 0x00C0) == 0x00C0;
    Logger::log("  bits [7:6] always-1: %s\n", ok ? "OK" : "FAIL");

    Logger::log("[INFO]  01 — status and HV counter printed above.\n");
}
