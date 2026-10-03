// The v8.2 ROM's own power-on self-tests (SBC1 0703 to the JMP 00CC at SBC2 0930), run by the
// firmware on the emulated machine; see docs/aru_fpc.md §3 and rom_map.md §5.
// Every failure the ROM reports through its error display (0217 'H', 021D 'E')
// is recorded, attributed to a test by the ROM's error type in D, and skipped
// with a RET, so the whole sequence runs to 00CC. The report lists each test,
// and each check of the DSP bus test, as passed or failed. ctest fails when a
// result differs from its expectation below. ROMs load from LEX224_ROM_DIR.
#include <cstdio>
#include <string>

#include "machine.h"
#include "romfile.h"

#ifndef LEX224_ROM_DIR
#error "LEX224_ROM_DIR must be defined"
#endif

static uint8_t sbc[0x1800], nvs[0x8000];
static int16_t dmem[65536];

// One check of SBC2 0A1D, identified by the address of its CNZ/CZ to the
// error chain 0B52-0B5B.
struct Check {
    uint16_t site;
    const char* what;
    bool expectPass;
    int failures = 0;
};

// Failing checks read IN 00-05 (microinstruction fields, bus test register,
// ARU state) or rely on the halted DSP decoding its parked step, neither of
// which the emulator models.
static Check busChecks[] = {
    {0x0A27, "IN 02 bus test register = 55 after OUT 07 55", false},
    {0x0A34, "IN 02 bus test register = AA after OUT 07 AA", false},
    {0x0A51, "single cycle: two passes of 0B65 copy X in to X out (IN 07 = 55)", true},
    {0x0A60, "halted: OUT 07 AA does not reach IN 07 (separate X registers)", true},
    {0x0A6A, "single cycle, no OUT 03: OUT 07 AA does not reach IN 07", true},
    {0x0A7B, "continuous run: X out follows X in (IN 07 = AA)", true},
    {0x0A94, "IN 00 = OFST0-7 of the parked step 127 (55)", false},
    {0x0A9E, "IN 05 & DF = I/O decode of step 127 (55)", false},
    {0x0AB0, "IN 00 = AA after 41FC <- AA", false},
    {0x0AB8, "IN 06 = AA: X passes through step 127's RD XREG + WR XREG", false},
    {0x0AC0, "IN 07 = AA: passes only because X out still holds AA from 0A7B", true},
    {0x0ACC, "IN 03 | 01 = A1 (ARU state bits)", false},
    {0x0ADB, "IN 06 = 55 after OUT 06/07 55", false},
    {0x0AE3, "IN 07 = 55 after OUT 06/07 55", false},
    {0x0AF0, "IN 04 | 20 = AA (WA, RA, CSIGN, RD AD, MEMW, RESET/)", false},
    {0x0AFA, "IN 01 = 5A (OFST8-13, XFER, CAS)", false},
    {0x0B08, "IN 01 = A5 after 41FD <- A5", false},
    {0x0B12, "IN 05 = AA (DAC selects, WR DA, X and result reads)", false},
    {0x0B24, "IN 04 & DF = 55 after 41FE <- 95", false},
    {0x0B35, "IN 03 & FE = 54 after 41FF <- CA", false},
};

struct Test {
    const char* name;
    uint16_t entry;
    uint8_t type;  // error type the ROM passes in D (high nibble)
    Check* checks;
    int nChecks;
    bool ran = false;
    int failures = 0;
};

static Test tests[] = {
    {"ROM checksums", 0x0703, 0x00, nullptr, 0},
    {"NVS RAM", 0x0864, 0x10, nullptr, 0},
    {"SBC RAM", 0x0897, 0xD0, nullptr, 0},
    {"WCS RAM", 0x08F9, 0x20, nullptr, 0},
    {"DSP bus and single cycle", 0x0A1D, 0x30, busChecks, int(sizeof busChecks / sizeof *busChecks)},
    {"ARU register file", 0x0C48, 0x40, nullptr, 0},
    {"ARU multiplier", 0x0942, 0x80, nullptr, 0},
    {"DMEM 64K", 0x0B75, 0x90, nullptr, 0},
};

// Machine whose 8080 sees a RET at the error-display entries, after the
// report is recorded. Opcode and operand fetches are reads at pc - 1 (as in
// tools/lexprobe.cpp).
struct SelfTestMachine : Machine {
    int unattributed = 0, unknownSites = 0;

    uint8_t read(uint16_t a) override {
        const uint8_t v = Machine::read(a);
        if (uint16_t(a + 1) != cpu.pc) return v;
        for (Test& t : tests)
            if (a == t.entry) t.ran = true;
        if (a != 0x0217 && a != 0x021D) return v;

        Test* t = nullptr;
        for (Test& x : tests)
            if ((cpu.d & 0xF0) == x.type) t = &x;
        if (!t) {
            std::printf("  unknown error type: D=%02X A=%02X B=%02X C=%02X\n", cpu.d, cpu.a, cpu.b, cpu.c);
            unattributed++;
            return 0xC9;
        }
        t->failures++;
        if (t->checks) {
            // Stack: return into 0B63, the BC pushed at 0B5B, then the return
            // address after the check's 3-byte CNZ/CZ.
            const uint16_t site = uint16_t(sbcRamWord(uint16_t(cpu.sp + 4)) - 3);
            bool found = false;
            for (int i = 0; i < t->nChecks; i++)
                if (t->checks[i].site == site) {
                    t->checks[i].failures++;
                    found = true;
                }
            if (!found) {
                std::printf("  %s: error from unlisted site %04X\n", t->name, site);
                unknownSites++;
            }
        } else {
            std::printf("  %s: %c error D=%02X A=%02X B=%02X C=%02X\n", t->name,
                        a == 0x0217 ? 'H' : 'E', cpu.d, cpu.a, cpu.b, cpu.c);
        }
        return 0xC9;  // RET
    }
};

static SelfTestMachine m;

int main() {
    std::string err;
    if (!loadRomFiles(LEX224_ROM_DIR, sbc, nvs, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    m.reset(RomImage{sbc, nvs}, dmem);
    std::printf("ROM power-on self-tests, 0703 to 00CC\n");
    if (!m.runFrom(0x0703, 0x00CC, 60.0)) {
        std::printf("FAIL: the self-test sequence did not reach 00CC\n");
        return 1;
    }

    int bad = m.unattributed + m.unknownSites;
    for (const Test& t : tests) {
        bool expectPass = true;
        for (int i = 0; i < t.nChecks; i++) expectPass = expectPass && t.checks[i].expectPass;
        const bool passed = t.ran && t.failures == 0;
        const bool ok = t.ran && passed == expectPass;
        if (!ok) bad++;
        std::printf("%-5s %-26s (%04X)", passed ? "pass" : "FAIL", t.name, t.entry);
        if (!t.ran)
            std::printf("  NOT RUN");
        else if (!passed)
            std::printf("  %d error report(s)%s", t.failures, ok ? ", expected" : "");
        if (!ok && t.ran) std::printf("  UNEXPECTED");
        std::printf("\n");
        for (int i = 0; i < t.nChecks; i++) {
            const Check& c = t.checks[i];
            const bool cPassed = c.failures == 0;
            const bool cOk = cPassed == c.expectPass;
            if (!cOk) bad++;
            std::printf("      %-4s %04X  %s%s\n", cPassed ? "pass" : "FAIL", c.site, c.what,
                        cOk ? (cPassed ? "" : "  (expected: not modelled)") : "  UNEXPECTED");
        }
    }
    std::printf("%d result(s) differ from expectations\n", bad);
    return bad ? 1 : 0;
}
