# lex224: the lex224emu engine (emu/src) as a Terrarium pedal on the Daisy Seed.
TARGET = lex224

# The app runs from SRAM, loaded from QSPI by the Daisy bootloader.
export APP_TYPE = BOOT_SRAM

# Source lists must be set before including the core Makefile: it evaluates
# vpath directives at parse time.
CPP_SOURCES = src/lex224_pedal.cpp src/crash_trace.cpp ../emu/src/engine.cpp \
              ../emu/src/machine.cpp ../emu/src/dsp.cpp ../emu/src/i8080.cpp \
              ../emu/src/resampler.cpp
# EMBED_ROMS=0 builds without the V8.2 ROM images (the editor flashes them into
# QSPI). Its objects live in their own directory so switching never links stale
# objects; the core Makefile's plain BUILD_DIR assignment yields to the override.
EMBED_ROMS ?= 1
ifeq ($(EMBED_ROMS),1)
ASM_SOURCES = src/roms.s
else
override BUILD_DIR = build_noroms
endif

# libDaisy is the daisy/libDaisy submodule (v8.1.0); terrarium.h is vendored in src/.
LIBDAISY_DIR ?= libDaisy
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
# libDaisy's BOOT_SRAM script plus a .sram_bss section: DMEM lives in AXI SRAM
# (measured ~80 permille less load than SDRAM).
LDSCRIPT = STM32H750IB_sram_lex224.lds

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

C_INCLUDES += -I../emu/src
C_DEFS += -DLEX_EMBED_ROMS=$(EMBED_ROMS)
ifeq ($(EMBED_ROMS),1)
# roms.s .incbin's the ROM files by name.
ASFLAGS += -Wa,-I,$(CURDIR)/../roms/224XL_v8_21
endif

# Host unit tests for the host-portable control logic and editor protocol (no firmware build needed).
HOSTCXX ?= g++
$(BUILD_DIR)/pedal_controls_test: tests/pedal_controls_test.cpp tests/check.h src/pedal_controls.h src/editor_protocol.h | $(BUILD_DIR)
	$(HOSTCXX) -std=gnu++17 -O1 -Wall -Wextra -Isrc -o $@ $<
$(BUILD_DIR)/editor_protocol_test: tests/editor_protocol_test.cpp tests/check.h src/editor_protocol.h | $(BUILD_DIR)
	$(HOSTCXX) -std=gnu++17 -O1 -Wall -Wextra -Isrc -o $@ $<
test: $(BUILD_DIR)/pedal_controls_test $(BUILD_DIR)/editor_protocol_test
	./$(BUILD_DIR)/pedal_controls_test
	./$(BUILD_DIR)/editor_protocol_test
.PHONY: test
