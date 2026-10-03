// Crash recovery for the lex224 pedal. Every way the pedal can stop ends in a reset that leaves a record in
// backup SRAM (kept across resets while powered), which the main loop prints over USB serial:
// - a CPU fault: the HardFault, MemManage, BusFault and UsageFault vectors save the stacked PC/LR/xPSR and
//   the fault status registers, then reset. libDaisy's own handlers never return: HardFault ends in a
//   breakpoint, and MemManage (enabled by HAL_MPU_Enable) is the startup file's `for (;;);` at priority 0.
// - an audio stall: the main loop sees the callback's block count stop for 100 ms, saves the SAI1/DMA1 state,
//   then resets;
// - anything else that stops the main loop (a bus stall, a CPU lockup): the independent watchdog resets, and
//   the next boot records the reset flags and the callback's last breadcrumbs (gTrace).
#pragma once
#include <cstdint>

namespace crash {

enum Kind : uint32_t { kNone = 0, kFault = 1, kStall = 2, kReset = 3 };

// Written by the audio callback at the end of every block.
struct Trace {
    uint32_t magic;
    uint32_t blocks;    // callbacks completed
    int32_t program;    // program index (gProgIndex)
    int32_t state;      // gState
    uint32_t nowMs;     // System::GetNow()
};

// The last event since power-on.
struct Event {
    uint32_t magic;
    uint32_t count;     // events since power-on
    uint32_t kind;      // Kind
    uint32_t pending;   // set by the fault/stall path right before its own reset
    uint32_t rsr;       // RCC->RSR read at the boot after the event
    Trace trace;        // breadcrumbs at the event
    // kFault: stacked PC/LR/xPSR of the faulting code (xPSR bits 8:0 = its exception: 0 = main loop,
    // 27/28 = DMA1 stream 0/1 = SAI1 A/B, the receive one runs the audio callback), the SCB fault status
    // registers (HFSR 0 = taken directly, not escalated) and EXC_RETURN.
    uint32_t pc, lr, xpsr, cfsr, hfsr, mmfar, bfar, excReturn;
    // kFault, kStall: DMA1 low interrupt status, streams 0/1 (SAI1 A/B) CR and NDTR, SAI1 A/B status.
    uint32_t dmaLisr, dma0Cr, dma0Ndtr, dma1Cr, dma1Ndtr, saiASr, saiBSr;
};

extern volatile Trace gTrace;
extern volatile Event gEvent;

// After hw.Init(): opens backup SRAM, records a reset that no fault/stall path explains (with the reset
// flags), and routes the four fault vectors here.
void Init();
// Independent watchdog, ~timeoutMs (LSI 32 kHz / 64); cannot be stopped until reset.
void StartWatchdog(uint32_t timeoutMs);
void KickWatchdog();
// Records an audio stall and resets.
[[noreturn]] void StallReset();

}  // namespace crash
