#include "crash_trace.h"

#include "daisy_seed.h"

namespace crash {

// Backup SRAM: libDaisy's MPU region (mpu.h configureMpu keeps it) maps it uncached, so every
// write lands before a reset (except inside HardFault, see finishEvent). The .backup_sram.* input sections
// link after .backup_sram, which keeps libDaisy's boot_info (shared with the bootloader) at the start.
__attribute__((section(".backup_sram.lex"))) volatile Trace gTrace;
__attribute__((section(".backup_sram.lex"))) volatile Event gEvent;

namespace {

constexpr uint32_t kMagic = 0x4C455831;   // "LEX1"; change it whenever the records' layout or meaning changes
constexpr int kVectorCount = 16 + WAKEUP_PIN_IRQn + 1;

// Copy of the vector table with our fault entries (.bss = DTCM; VTOR needs 1 KiB alignment here).
alignas(1024) uint32_t gVectors[kVectorCount];

void copyTrace() {
    gEvent.trace.magic = gTrace.magic;
    gEvent.trace.blocks = gTrace.blocks;
    gEvent.trace.program = gTrace.program;
    gEvent.trace.state = gTrace.state;
    gEvent.trace.nowMs = gTrace.nowMs;
}

void recordAudioDma() {
    gEvent.dmaLisr = DMA1->LISR;
    gEvent.dma0Cr = DMA1_Stream0->CR;
    gEvent.dma0Ndtr = DMA1_Stream0->NDTR;
    gEvent.dma1Cr = DMA1_Stream1->CR;
    gEvent.dma1Ndtr = DMA1_Stream1->NDTR;
    gEvent.saiASr = SAI1_Block_A->SR;
    gEvent.saiBSr = SAI1_Block_B->SR;
}

void clearFaultRegisters() {
    gEvent.pc = gEvent.lr = gEvent.xpsr = gEvent.cfsr = gEvent.hfsr = 0;
    gEvent.mmfar = gEvent.bfar = gEvent.excReturn = 0;
}

void clearAudioDma() {
    gEvent.dmaLisr = gEvent.dma0Cr = gEvent.dma0Ndtr = gEvent.dma1Cr = gEvent.dma1Ndtr = 0;
    gEvent.saiASr = gEvent.saiBSr = 0;
}

// First: pending, count and kind.
void beginEvent(Kind kind) {
    gEvent.pending = 1;
    gEvent.count = gEvent.count + 1;
    gEvent.kind = kind;
}

// libDaisy enables the MPU without HFNMIENA, so inside HardFault the MPU is off and backup SRAM falls back to
// the default map: write-back cached. Without the clean, a HardFault record stays in the D-cache and the
// reset drops it (seen on the device).
[[noreturn]] void finishEvent() {
    copyTrace();
    SCB_CleanDCache();
    __DSB();
    NVIC_SystemReset();
}

}  // namespace

extern "C" __attribute__((used, noreturn)) void RmxFaultRecord(const uint32_t* frame, uint32_t excReturn) {
    beginEvent(kFault);
    gEvent.pc = frame[6];
    gEvent.lr = frame[5];
    gEvent.xpsr = frame[7];
    gEvent.cfsr = SCB->CFSR;
    gEvent.hfsr = SCB->HFSR;
    gEvent.mmfar = SCB->MMFAR;
    gEvent.bfar = SCB->BFAR;
    gEvent.excReturn = excReturn;
    recordAudioDma();
    finishEvent();
}

// The stacked frame is on MSP or PSP (EXC_RETURN bit 2).
extern "C" __attribute__((naked, used)) void RmxFaultEntry() {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mov r1, lr\n"
        "b RmxFaultRecord\n");
}

void Init() {
    daisy::System::InitBackupSram();
    const uint32_t rsr = RCC->RSR;
    RCC->RSR = RCC->RSR | RCC_RSR_RMVF;

    if (gEvent.magic != kMagic) {   // first boot after power-on
        gEvent.magic = kMagic;
        gEvent.count = 0;
        gEvent.kind = kNone;
        gEvent.pending = 0;
        gEvent.rsr = 0;
        clearFaultRegisters();
        clearAudioDma();
    }
    if (gTrace.magic == kMagic) {   // the previous run on this power-up ended in a reset
        if (gEvent.pending) {
            gEvent.pending = 0;      // our own fault/stall reset: already recorded
        } else {                     // watchdog, reset pin, DFU, brown-out: rsr tells which
            gEvent.kind = kReset;
            copyTrace();
            clearFaultRegisters();
            clearAudioDma();
            gEvent.count = gEvent.count + 1;
        }
        gEvent.rsr = rsr;
    }
    gTrace.blocks = 0;
    gTrace.program = -1;
    gTrace.state = 0;
    gTrace.nowMs = 0;
    gTrace.magic = kMagic;

    const uint32_t* table = reinterpret_cast<const uint32_t*>(SCB->VTOR);
    for (int i = 0; i < kVectorCount; i++) gVectors[i] = table[i];
    // HardFault and the three configurable faults. HAL_MPU_Enable (libDaisy's System::ConfigureMpu and
    // mpu.h configureMpu) turns on the MemManage exception, whose default handler is `for (;;);` at priority 0.
    for (int i = 3; i <= 6; i++) gVectors[i] = reinterpret_cast<uint32_t>(&RmxFaultEntry);
    __disable_irq();
    SCB->VTOR = reinterpret_cast<uint32_t>(gVectors);
    __DSB();
    __ISB();
    __enable_irq();
}

void StartWatchdog(uint32_t timeoutMs) {
    uint32_t reload = timeoutMs / 2;   // LSI 32 kHz / 64 = 2 ms per count
    if (reload > 0xFFF) reload = 0xFFF;
    IWDG1->KR = 0xCCCC;   // start
    IWDG1->KR = 0x5555;   // unlock PR/RLR
    IWDG1->PR = 4;        // /64
    IWDG1->RLR = reload;
    while (IWDG1->SR != 0) {
    }
    IWDG1->KR = 0xAAAA;
}

void KickWatchdog() { IWDG1->KR = 0xAAAA; }

void StallReset() {
    beginEvent(kStall);
    clearFaultRegisters();
    recordAudioDma();
    finishEvent();
}

}  // namespace crash
