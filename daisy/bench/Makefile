# lexbench: a standalone on-device benchmark/profiler for the shared
# lex224 engine (emu/src), built the same way as daisy/Makefile but with no
# audio codec use. See bench.cpp for what it measures and prints.
TARGET = lexbench

export APP_TYPE = BOOT_SRAM

# Source lists must be set before including the core Makefile: it evaluates
# vpath directives at parse time.
CPP_SOURCES = bench.cpp ../../emu/src/engine.cpp ../../emu/src/machine.cpp \
              ../../emu/src/dsp.cpp ../../emu/src/i8080.cpp ../../emu/src/resampler.cpp
# Reuses the pedal's ROM-embedding object; not modified here.
ASM_SOURCES = ../src/roms.s

# libDaisy is the daisy/libDaisy submodule, same as daisy/Makefile.
LIBDAISY_DIR ?= ../libDaisy
# Stop with the fix if the libDaisy submodule, or its nested driver submodules, is not checked out.
ifeq ($(wildcard $(LIBDAISY_DIR)/core/Makefile),)
$(error libDaisy not found at $(LIBDAISY_DIR). From the repo root run: git submodule update --init --recursive && make -C daisy/libDaisy)
endif
ifeq ($(wildcard $(LIBDAISY_DIR)/Drivers/STM32H7xx_HAL_Driver/Inc/stm32h7xx_hal.h),)
$(error libDaisy's driver submodules are missing under $(LIBDAISY_DIR). From the repo root run: git submodule update --init --recursive && make -C daisy/libDaisy)
endif

OPT = -O3
# The shared core needs C++17 (std::clamp, std::gcd); GNU mode provides M_PI.
CPP_STANDARD = -std=gnu++17
# Same as daisy/Makefile: BOOT_SRAM plus a .sram_bss section, here used to
# hold a second DMEM copy in AXI SRAM (bench.cpp compares it against SDRAM).
LDSCRIPT = ../STM32H750IB_sram_lex224.lds

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

C_INCLUDES += -I../../emu/src
# roms.s .incbin's the ROM files by name.
ASFLAGS += -Wa,-I,$(CURDIR)/../../roms/224XL_v8_21
