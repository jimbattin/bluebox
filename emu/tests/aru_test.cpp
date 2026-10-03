// ARU arithmetic against the v8.2 ROM's own DSP self-tests (docs/aru_fpc.md §3):
// the multiplier test SBC2 0942 and the register-file test SBC2 0C48. Programs,
// inputs and expected words come from the ROM image; each `OUT 03` of the
// self-test is one pass of the test program. ROMs load from LEX224_ROM_DIR.
#include <cstdio>
#include <string>

#include "dsp.h"
#include "romfile.h"

#ifndef LEX224_ROM_DIR
#error "LEX224_ROM_DIR must be defined"
#endif

static uint8_t sbc[0x1800], nvs[0x8000];
static int16_t dmem[65536];
static Dsp dsp;
static int failures = 0;

static uint16_t be16(uint16_t a) { return uint16_t(sbc[a] << 8 | sbc[a + 1]); }

// SBC2 0933: copies the program at `src` into 41FF, 41FE, ... down to (41xx | stop) + 1.
static void loadProgram(uint16_t src, uint8_t stop) {
    for (unsigned a = 0x41FF; (a & 0xFF) != stop; a--, src++) dsp.writeWcs(uint16_t(a - 0x4000), sbc[src]);
}

// One `OUT 03` in single-cycle mode.
static void pass() {
    while (!dsp.run(128, false).boundary) {
    }
}

static void check(const char* test, int group, uint16_t in, uint16_t expected) {
    const uint16_t got = uint16_t(dsp.xout);
    if (got == expected) return;
    std::printf("%s group %d x=%04X: expected %04X got %04X\n", test, group, in, expected, got);
    failures++;
}

// SBC2 0942: program 0A0D (steps 127-124). Step 126 reads X into register 3,
// clears the accumulator and multiplies register 3 by the coefficient in
// 41FA/41FB; step 125 transfers, step 124 writes the result to X. The harness
// 099A writes X (high byte first), runs one pass and compares X with the next
// big-endian word from 09E5.
static void multiplierTest() {
    dsp.reset(dmem);
    loadProgram(0x0A0D, 0xEF);
    struct Group {
        uint8_t b41FA, b41FB;  // 0 = keep the previous byte (MVI A/STA at 0955-0983)
        uint16_t inputs;
    };
    static const Group groups[] = {
        {0, 0, 0x09D5}, {0xFE, 0, 0x09D5}, {0x7E, 0x55, 0x09D5}, {0xFE, 0, 0x09D5}, {0, 0x01, 0x09DD},
    };
    uint16_t expected = 0x09E5;
    int g = 0;
    for (const Group& gr : groups) {
        g++;
        if (gr.b41FA) dsp.writeWcs(0x1FA, gr.b41FA);
        if (gr.b41FB) dsp.writeWcs(0x1FB, gr.b41FB);
        for (int k = 0; k < 4; k++, expected += 2) {
            const uint16_t in = be16(uint16_t(gr.inputs + 2 * k));
            dsp.xin = int16_t(in);
            pass();
            check("multiplier", g, in, be16(expected));
        }
    }
}

// SBC2 0C48: program 0CDB (steps 127-123). Step 126 reads X into register WA
// and multiplies register RA by +1; 41FA selects WA/RA. Each table (0CC3,
// 0CCF) holds four (write 41FA, read 41FA, X) triples: 0C7B writes X into
// registers 0-3, then 0C91 reads registers 0-3 back through the multiplier.
// The last read (WA = RA = 3) passes this pass's X input through, which is
// still the value 0C7B wrote: X in and X out are separate registers.
static void registerFileTest() {
    dsp.reset(dmem);
    loadProgram(0x0CDB, 0xEB);
    int g = 0;
    for (const uint16_t table : {uint16_t(0x0CC3), uint16_t(0x0CCF)}) {
        g++;
        for (int k = 0; k < 4; k++) {
            const uint8_t* t = &sbc[table + 3 * k];
            dsp.writeWcs(0x1FA, t[0]);
            dsp.xin = int16_t(t[2] << 8 | t[2]);
            pass();
        }
        for (int k = 0; k < 4; k++) {
            const uint8_t* t = &sbc[table + 3 * k];
            dsp.writeWcs(0x1FA, t[1]);
            pass();
            check("register file", g, uint16_t(dsp.xin), uint16_t(t[2] << 8 | t[2]));
        }
    }
}

int main() {
    std::string err;
    if (!loadRomFiles(LEX224_ROM_DIR, sbc, nvs, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    multiplierTest();
    registerFileTest();
    std::printf("%d mismatches\n", failures);
    return failures ? 1 : 0;
}
