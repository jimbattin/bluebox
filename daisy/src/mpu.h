// MPU setup shared by the pedal and the bench, called right after hw.Init().
//
// ST's background MPU region for the STM32H7 (AN4838; STM32CubeMX's default): no access, execute-never and
// strongly ordered over the external-memory window 0x60000000-0xDFFFFFFF (subregions 3-6 of a 4 GB region).
// The Cortex-M7 reads Normal memory speculatively, through whatever value a register holds (ES0392 2.1.2);
// libDaisy v8.1.0 leaves this window on the default map, which includes the 8 MB memory-mapped QSPI region.
// A repeated read of its last byte, 0x907FFFFF, stalls the AXI bus (ES0392 2.7.5), and no interrupt can
// break the stall. The emulator's 64-bit master tick counters (cpuTicks/dspTicks, 30.72 MHz) have a low word
// that passes 0x907FFFFF every 2.3 min. A no-access region is never read speculatively; a real access raises
// MemManage (recorded by the pedal, crash_trace.h). libDaisy's three regions (System::ConfigureMpu) follow
// unchanged, then the pedal's ROM store window. Higher region numbers win where regions overlap.
#pragma once
#include "daisy_seed.h"

inline void configureMpu() {
    HAL_MPU_Disable();
    MPU_Region_InitTypeDef r{};
    r.Enable = MPU_REGION_ENABLE;

    r.Number = MPU_REGION_NUMBER0;
    r.BaseAddress = 0x00000000;
    r.Size = MPU_REGION_SIZE_4GB;
    r.SubRegionDisable = 0x87;   // keep the default map for 0x00000000-0x5FFFFFFF and 0xE0000000-0xFFFFFFFF
    r.AccessPermission = MPU_REGION_NO_ACCESS;
    r.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    r.TypeExtField = MPU_TEX_LEVEL0;
    r.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    r.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
    r.IsShareable = MPU_ACCESS_SHAREABLE;
    HAL_MPU_ConfigRegion(&r);

    r.SubRegionDisable = 0x00;
    r.AccessPermission = MPU_REGION_FULL_ACCESS;
    r.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;

    // libDaisy region 0: D2 SRAM1, the DMA buffers, uncached.
    r.Number = MPU_REGION_NUMBER1;
    r.BaseAddress = 0x30000000;
    r.Size = MPU_REGION_SIZE_32KB;
    r.TypeExtField = MPU_TEX_LEVEL1;
    r.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    r.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
    r.IsShareable = MPU_ACCESS_SHAREABLE;
    HAL_MPU_ConfigRegion(&r);

    // libDaisy region 1: SDRAM, write-back cached (the bench keeps gDmem and gHist there).
    r.Number = MPU_REGION_NUMBER2;
    r.BaseAddress = 0xC0000000;
    r.Size = MPU_REGION_SIZE_64MB;
    r.TypeExtField = MPU_TEX_LEVEL0;
    r.IsCacheable = MPU_ACCESS_CACHEABLE;
    r.IsBufferable = MPU_ACCESS_BUFFERABLE;
    r.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
    HAL_MPU_ConfigRegion(&r);

    // libDaisy region 2: backup SRAM, uncached (libDaisy's boot_info, the pedal's crash records).
    r.Number = MPU_REGION_NUMBER3;
    r.BaseAddress = 0x38800000;
    r.Size = MPU_REGION_SIZE_4KB;
    r.TypeExtField = MPU_TEX_LEVEL1;
    r.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    r.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
    r.IsShareable = MPU_ACCESS_SHAREABLE;
    HAL_MPU_ConfigRegion(&r);

    // The ROM store window: the pedal reads its ROM slots and the mono-output store memory-mapped
    // (romSlotFlash, loadOutMixStore). Subregions 0-5, 0x907F0000-0x907FBFFF, cover slots 0-10 and the
    // mono-output store (up to 0x907FBFFF); subregions 6-7 fall back to the no-access background, which
    // keeps the stalling last byte blocked. Write-back cached as on the default map, so
    // romSlotFlash's cache invalidate still applies. Read-only: flash writes and erases go through the QSPI
    // registers (indirect mode), not this window.
    r.Number = MPU_REGION_NUMBER4;
    r.BaseAddress = 0x907F0000;
    r.Size = MPU_REGION_SIZE_64KB;
    r.SubRegionDisable = 0xC0;
    r.AccessPermission = MPU_REGION_PRIV_RO;
    r.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    r.TypeExtField = MPU_TEX_LEVEL1;
    r.IsCacheable = MPU_ACCESS_CACHEABLE;
    r.IsBufferable = MPU_ACCESS_BUFFERABLE;
    r.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
    HAL_MPU_ConfigRegion(&r);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}
