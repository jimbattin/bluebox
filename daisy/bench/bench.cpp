// lex224 on-device engine bench: a standalone STM32H750 program (no audio
// codec) that measures the shared `emu/src` engine on the real Seed hardware
// and profiles it. It boots each of the 22 factory programs, feeds a fixed
// synthetic sine block, and reports (over USB serial, like the pedal's log):
//
//   B  ...   one-off timing: DSP loop length/rate, and permille of a 480 MHz
//            second spent in each stage (input resampler, output resampler,
//            Machine::run, the DSP-only inner loop, engine end to end), plus
//            cycles/DSP-step with DMEM in SDRAM vs. AXI SRAM.
//   P  name  per-program: boot time, average/max callback load in permille
//            of the block period, and output-FIFO/input underruns.
//   S  ...   PC-sampling profiler totals: samples taken and samples outside
//            the tracked .text range ("other").
//   H  addr count
//            one PC-sampling histogram bucket: `addr` is the bucket's base
//            address (8 bytes/bucket, see kShift) in the BOOT_SRAM .text
//            range (0x24000000+), `count` is samples landing in it. Feed
//            these through `daisy/bench/symbolize.py` to attribute them to
//            functions.
//
// The profiler samples the program counter from a TIM7 interrupt at ~20 kHz
// while `Engine::process` runs on CONCERT HALL for 5 s (see profInit()/
// prof_sample()). It needs `Engine`/`Machine`/`Dsp` internals (the engine's
// own Machine instance, and the DSP's raw dmem pointer and batch `run()`) not
// exposed by Engine's public API. `Machine`'s members are `protected` (for
// tools/lexprobe.cpp's benefit), but `Engine`'s are still private with no
// bench-specific accessor, so both `#define private public` and
// `#define protected public` are needed here, for this include only.
//
// The whole run repeats 4 times (so a log reader has time to attach) and
// then resets the Seed into its DFU bootloader; it never returns to the
// pedal firmware. Flash it like the pedal (see daisy/README.md), read its
// log with `daisy/tools/cdc_read.py`, and rebuild/reflash the pedal
// afterwards -- this binary replaces it, it does not run alongside it.
#include <cstdint>
#include <cmath>

#define private public      // bench-only access to Engine internals
#define protected public    // bench-only access to Machine internals
#include "engine.h"
#undef private
#undef protected
#include "daisy_seed.h"
#include "../src/mpu.h"

using namespace daisy;

extern "C" const uint8_t lex_rom_sbc[0x1800], lex_rom_nvs[0x8000];

static DaisySeed hw;
static Engine<1, 1> gEngine;
static StreamResampler<1, 1024> gRs;
DSY_SDRAM_BSS static int16_t gDmem[65536];
// Second DMEM copy in AXI SRAM (see STM32H750IB_sram_lex224.lds' .sram_bss),
// to compare DSP throughput with DMEM in SDRAM vs. AXI SRAM.
alignas(32) __attribute__((section(".sram_bss"))) static int16_t gDmemSram[65536];
DTCM_MEM_SECTION static float gKernel[kKernelTableSize];
static ProgramList gPrograms;

// --- PC-sampling profiler ---
constexpr uint32_t kTextBase = 0x24000000, kTextRange = 0x40000, kShift = 3;
DSY_SDRAM_BSS static uint32_t gHist[kTextRange >> kShift];
static volatile uint32_t gOther, gSamples;
static volatile bool gProfOn;

extern "C" void prof_sample(uint32_t pc) {
    TIM7->SR = 0;
    if (gProfOn) {
        gSamples = gSamples + 1;
        const uint32_t off = pc - kTextBase;
        if (off < kTextRange)
            gHist[off >> kShift]++;
        else
            gOther = gOther + 1;
    }
    __DSB();
}
extern "C" __attribute__((naked)) void TIM7_IRQHandler() {
    asm volatile("ldr r0, [sp, #24]\n\tb prof_sample\n");
}
static void profInit() {
    // gHist is in SDRAM, which the startup code does not zero.
    for (uint32_t& h : gHist) h = 0;
    RCC->APB1LENR |= RCC_APB1LENR_TIM7EN;
    (void)RCC->APB1LENR;
    TIM7->PSC = 0;
    TIM7->ARR = 11993;  // ~20 kHz, not a divisor of the audio/sample periods
    TIM7->DIER = TIM_DIER_UIE;
    TIM7->CR1 = TIM_CR1_CEN;
    NVIC_SetPriority(TIM7_IRQn, 0);
    NVIC_EnableIRQ(TIM7_IRQn);
}

struct NullIo final : SampleIo {
    bool nextInput(int16_t& l, int16_t& r) override { l = r = 0; return true; }
    void frame(const DacFrame&) override {}
};

static int pm(uint32_t ticks) { return int(float(ticks) * 1000.0f / float(System::GetTickFreq())); }

static float in[48], out[48];
static const float* ip[1] = {in};
static float* op[1] = {out};

static void feed(int b) {
    for (int i = 0; i < 48; i++) in[i] = 0.3f * sinf(float((b * 48 + i) % 48000) * 0.0287979f);
}

int main() {
    __set_FPSCR(__get_FPSCR() | (1u << 24));
    hw.Init(true);
    configureMpu();
    hw.StartLog(false);
    initKernelTable(gKernel);
    listPrograms({lex_rom_sbc, lex_rom_nvs}, gPrograms);
    profInit();

    EngineConfig cfg{{lex_rom_sbc, lex_rom_nvs}, gDmem, gKernel, 48000, 0x01, 1, 1, {0x01}, 0, 0};  // one output: DAC A
    const char* err = nullptr;
    gEngine.start(cfg, &err);
    const int L = gEngine.loopLen();
    const int D = int(gEngine.dspRate());
    volatile float sink = 0;

    gRs.init(gKernel, 48000u * 9 * L, 30720000u);
    uint32_t s = System::GetTick();
    for (int i = 0; i < 48000; i++) {
        float x = float(i & 255) * (1.0f / 256.0f), y;
        gRs.push(&x);
        while (gRs.ready()) { gRs.pull(&y); sink = sink + y; }
    }
    const uint32_t tRsIn = System::GetTick() - s;

    gRs.init(gKernel, 30720000u, 48000u * 9 * L);
    s = System::GetTick();
    for (int i = 0; i < D; i++) {
        float x = float(i & 255) * (1.0f / 256.0f), y;
        gRs.push(&x);
        while (gRs.ready()) { gRs.pull(&y); sink = sink + y; }
    }
    const uint32_t tRsOut = System::GetTick() - s;

    NullIo nio;
    s = System::GetTick();
    gEngine.machine_.run(size_t(D), nio);
    const uint32_t tRun = System::GetTick() - s;

    Dsp& dsp = gEngine.machine_.dsp;
    const int dspSteps = 30720000 / 9;
    s = System::GetTick();
    for (int done = 0; done < dspSteps;) done += dsp.run(dspSteps - done, false).steps;
    const uint32_t tDsp = System::GetTick() - s;

    // Same DSP loop with DMEM in AXI SRAM instead of SDRAM.
    for (int i = 0; i < 65536; i++) gDmemSram[i] = dsp.dmem[i];
    dsp.dmem = gDmemSram;
    s = System::GetTick();
    for (int done = 0; done < dspSteps;) done += dsp.run(dspSteps - done, false).steps;
    const uint32_t tDspSram = System::GetTick() - s;
    cfg.dmem = gDmemSram;  // everything below runs with DMEM in AXI SRAM

    // Profile: CONCERT HALL, 5 s of audio.
    gEngine.start(cfg, &err);
    gProfOn = true;
    s = System::GetTick();
    for (int b = 0; b < 5000; b++) {
        feed(b);
        gEngine.process(ip, op, 48);
    }
    const uint32_t tProc = (System::GetTick() - s) / 5;
    gProfOn = false;

    // All programs: 1 s each.
    static int pmAvg[32], pmMax[32], bootMs[32];
    static uint32_t und[32];
    for (int p = 0; p < gPrograms.count; p++) {
        EngineConfig c = cfg;
        c.programId = gPrograms.items[p].id;
        const uint32_t t0 = System::GetNow();
        gEngine.start(c, &err);
        bootMs[p] = int(System::GetNow() - t0);
        uint32_t sum = 0, mx = 0;
        for (int b = 0; b < 1000; b++) {
            feed(b);
            const uint32_t t = System::GetTick();
            gEngine.process(ip, op, 48);
            const uint32_t d = System::GetTick() - t;
            sum += d;
            if (d > mx) mx = d;
        }
        pmAvg[p] = int(float(sum) / 240000.0f);
        pmMax[p] = int(float(mx) * 1000.0f / 240000.0f);
        und[p] = gEngine.underruns();
    }

    // Top profile buckets.
    constexpr int kTop = 60;
    static uint32_t topIdx[kTop], topCnt[kTop];
    for (uint32_t i = 0; i < (kTextRange >> kShift); i++) {
        const uint32_t cnt = gHist[i];
        if (cnt <= topCnt[kTop - 1]) continue;
        int j = kTop - 1;
        while (j > 0 && topCnt[j - 1] < cnt) {
            topCnt[j] = topCnt[j - 1];
            topIdx[j] = topIdx[j - 1];
            j--;
        }
        topCnt[j] = cnt;
        topIdx[j] = i;
    }

    for (int rep = 0; rep < 4; rep++) {
        hw.PrintLine("B loop %d D %d | permille of 480 MHz for 1 s of audio:", L, D);
        hw.PrintLine("B rs_in %d rs_out %d machine_run %d dsp_only %d (8080+sched %d) engine %d",
                     pm(tRsIn), pm(tRsOut), pm(tRun), pm(tDsp), pm(tRun) - pm(tDsp), pm(tProc));
        hw.PrintLine("B cycles: per dsp step %d (DMEM SDRAM), %d (DMEM AXI SRAM); engine/programs below use SRAM",
                     int(float(tDsp) * 2.0f / float(dspSteps)), int(float(tDspSram) * 2.0f / float(dspSteps)));
        for (int p = 0; p < gPrograms.count; p++)
            hw.PrintLine("P %-12s boot %4d ms load avg %4d max %4d permille und %u",
                         gPrograms.items[p].name, bootMs[p], pmAvg[p], pmMax[p], unsigned(und[p]));
        hw.PrintLine("S samples %u other %u", unsigned(gSamples), unsigned(gOther));
        for (int i = 0; i < kTop && topCnt[i]; i++)
            hw.PrintLine("H %08x %u", unsigned(kTextBase + (topIdx[i] << kShift)), unsigned(topCnt[i]));
        System::Delay(3000);
    }
    System::ResetToBootloader(System::BootloaderMode::DAISY_INFINITE_TIMEOUT);
}
