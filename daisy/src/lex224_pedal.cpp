// lex224 Terrarium pedal: the Lexicon 224XL V8.2 firmware and DSP, emulated in
// real time on the Daisy Seed.
//
// Left input (mono) -> DSP L and R; mono wet signal (see kSumAC; the editor can
// override it per program) -> both outputs.
// Dry/wet is an equal-power blend, with each program's wet trimmed to the dry's
// loudness (kWetTrim), so the blend stays at unity from full dry to full wet.
// FS1: bypass (LED1 lit when active).
// FS2 tap: next program in the bank (re-boots the emulated machine; wet output is
// muted while it loads, LED2 lit). FS2 held: knobs switch to the secondary layer;
// the release does not advance if a secondary knob went live during the hold.
// Knobs drive the 224XL's own LARC sliders live (map in pedal_controls.h):
//   primary   1 dry/wet mix, 2 LF DECAY, 3 MID DECAY, 4 CROSSOVER, 5 TREBLE DECAY, 6 DEPTH
//   secondary 1 PREDELAY, 2 DIFFUSION, 3 DEFINITION, 4 CHORUS, 5 HF BANDWIDTH, 6 LF STOP DCY
// Every knob is parked after power-up, a program load and a layer change, and
// takes over once moved. Split programs get page-1 moves on both reverbs.
// Switch 1: MODE ENH (a flip reprograms the DSP: short glitch, tail cut).
// Switch 2: DECAY OPT. The editor sets these and DYN DECAY too. Every program load
// starts from the preset's own options, whatever the lever positions; after it, a
// lever flip sets its option to the lever's position and an editor command sets its
// option, live, until the next load.
// Switches 3/4: bank (00, 10, 01, 11) of programs 1-6, 7-12, 13-18, 19-22; a
// lever change loads program 1 of the new bank once stable for 300 ms.
// LED2 while running: N flashes for in-bank program N, then a pause.
// The Seed's USB port is a USB MIDI device for the BARC editor (editor/ at the repo root):
// SysEx commands (src/editor_protocol.h) select any program, move any slider of
// the loaded program, and set options, mix, bypass and the mono output mix; the
// callback applies them.
// The pedal reports its state and slider registers (every 20 ms, when changed)
// and its log (program loads, CPU load once a second) as SysEx too. With no host
// reading, messages are dropped and nothing else is affected.
// ROMs: the engine runs from a RAM copy of the eleven ROMs (SBC1-3, NVS1-8). The
// editor flashes each into its own QSPI sector (kRomFlashBase); a flashed image with
// its socket's byte sum overrides that slot's built-in image, and an erase brings the
// built-in one back. Built with EMBED_ROMS=0 (daisy/Makefile) the firmware has no
// built-in images: while a slot is empty the pedal outputs the dry signal only and
// LED2 blinks at 1 Hz (NoRoms); the editor shows NO ROMS.
// An MPU background region keeps the CPU's speculative reads out of the QSPI
// window's stalling last byte (mpu.h). Any CPU fault, audio stall or hang
// restarts the pedal and leaves a record that the log prints (crash_trace.h).
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "crash_trace.h"
#include "daisy_petal.h"
#include "editor_protocol.h"
#include "engine.h"
#include "hid/usb_midi.h"
#include "mpu.h"
#include "pedal_controls.h"
#include "sys/dma.h"
#include "terrarium.h"

using namespace daisy;
using namespace terrarium;

#ifndef LEX_EMBED_ROMS
#error "LEX_EMBED_ROMS must be 0 or 1 (daisy/Makefile EMBED_ROMS)"
#endif
#if LEX_EMBED_ROMS
extern "C" const uint8_t lex_rom_sbc[0x1800], lex_rom_nvs[0x8000];
#endif

namespace {

constexpr size_t kBlockSize = 48;
constexpr uint32_t kSampleRate = 48000;
constexpr uint8_t kOptionBits = Machine::kOptDynDecay | Machine::kOptModeEnh | Machine::kOptDecayOpt;

enum State : int { Loading = 0, Running = 1, StopRequested = 2, Stopped = 3, Failed = 4, NoRoms = 5 };

DaisyPetal hw;
Engine<1, 1> gEngine;
// AXI SRAM (.sram_bss, see STM32H750IB_sram_lex224.lds): cached like SDRAM but
// with much cheaper misses. Not zeroed at startup; Engine::start clears it.
alignas(32) __attribute__((section(".sram_bss"))) int16_t gDmem[65536];
// The ROM image the engine runs from, built by loadRomImage() while the engine is
// stopped. The same AXI SRAM the embedded images (.rodata) live in.
alignas(32) __attribute__((section(".sram_bss"))) uint8_t gRomSbc[0x1800];
alignas(32) __attribute__((section(".sram_bss"))) uint8_t gRomNvs[0x8000];
DTCM_MEM_SECTION float gKernel[kKernelTableSize];
float gWet[kBlockSize];
ProgramList gPrograms;
CpuLoadMeter gLoad;
Led gLed1, gLed2;
std::atomic<int> gState{Loading};
std::atomic<bool> gBypass{false};
std::atomic<int> gLoadRequest{-1};  // program index for the main loop to load
std::atomic<int> gProgIndex{0};

// Callback-only control state.
constexpr int kKnobIndex[kKnobs] = {Terrarium::KNOB_1, Terrarium::KNOB_2, Terrarium::KNOB_3,
                                    Terrarium::KNOB_4, Terrarium::KNOB_5, Terrarium::KNOB_6};
KnobTakeover gKnobs;
Fs2Gesture gFs2;
BankSelector gBankSel;
int gLayer = 0;
int gLastSent[kKnobs];  // slider position last sent per knob, -1 since the last park
float gMix = 0.0f;
bool gMixPrimed = false;
uint8_t gAppliedOptions = 0;  // option bits the running program has
int gPrevState = Loading;

std::atomic<uint8_t> gLeverOptions{0};  // MODE ENH from SWITCH_1, DECAY OPT from SWITCH_2
OptionState gOptions;                   // callback-only after main() starts the audio
uint32_t gFlashStartMs = 0;
std::atomic<float> gWetTrim{1.0f};  // this program's kWetTrim as a linear gain

// BARC editor link. USB IRQ -> main loop: raw MIDI bytes; main loop -> callback:
// parsed commands. Main loop only: the parser, the send buffers and the send state.
MidiUsbTransport gMidi;
barc::SpscQueue<uint8_t, 512> gRx;
barc::SpscQueue<barc::Command, 32> gCommands;
barc::Parser gParser;
barc::TxBuffer gTx[2];  // gTx[gFill] fills while the other may be in flight
int gFill = 0;
uint16_t gPageMask = 0;  // bit p-1: page p present in the running program

// ROM store. Slot n (0-2 SBC1-3, 3-10 NVS1-8) is the 4 KiB QSPI sector at chip offset
// kRomFlashBase + n * 0x1000, in the last 64 KiB of the 8 MiB flash: the Daisy
// bootloader stores apps from 0x40000 and erases only the 64 KiB chunks a program (at
// most 480 KiB) covers, so the store survives program-dfu. The sector after the ROM
// slots holds the mono-output overrides (kOutMixFlashAddr). Main loop only. gUpload
// stays in .bss: .sram_bss is not initialised.
constexpr uint32_t kRomFlashBase = 0x7F0000;
barc::RomUpload gUpload;
uint8_t gRomState[barc::kRomSlots];  // barc::RomState
uint32_t gRomCrc[barc::kRomSlots];   // CRC-32 of the image in use, 0 if empty
bool gRomsDirty = true;              // ROMS to send (boot, HELLO, ROM changes)

// Mono-output overrides, saved in the sector after the ROM slots (0x7FB000); the table is
// main loop only.
constexpr uint32_t kOutMixFlashAddr = kRomFlashBase + barc::kRomSlots * 0x1000;
OutMixTable gOutOverrides;
std::atomic<uint8_t> gOutMixWanted{0x01};  // main loop -> callback: the outMix to apply
uint8_t gBootOutMix = 0x01;     // written by the main loop before gState = Running
uint8_t gAppliedOutMix = 0x01;  // callback-only: the engine's current outMix

// What the editor sees, taken by the callback (which owns the engine) on request:
// gSnapPhase 0 idle (main owns gSnap), 1 wanted (callback fills it), 2 ready (main reads it).
struct Snapshot {
    uint8_t sliders[12][6];
    uint8_t options;
    uint16_t mix;
    bool running;
};
Snapshot gSnap;
std::atomic<int> gSnapPhase{0};
Snapshot gLatest{};
barc::StateMsg gSentState{};
uint8_t gSent[12][6];
bool gSentValid[12] = {};
bool gForce = true;  // send everything at the next snapshot (HELLO, program load, 1 s)
uint32_t gLastSnapReq = 0;

// Non-blocking log, as LOG SysEx: appended to the send buffer (dropped if it is
// full) and sent by flushTx() from the main loop. Main loop only.
__attribute__((format(printf, 1, 2))) void logLine(const char* fmt, ...) {
    static char buf[128];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    gTx[gFill].AppendLog(buf);
}

// Starts the filled buffer's transfer if the USB endpoint is free. A successful
// start means the previous transfer finished, so the other buffer can be refilled;
// a busy endpoint keeps the batch for the next pass. Never blocks.
void flushTx() {
    barc::TxBuffer& t = gTx[gFill];
    if (t.size() == 0) return;
    // The transfer only reads the buffer; libDaisy's API is not const-correct.
    if (hw.seed.usb_handle.TransmitInternal(const_cast<uint8_t*>(t.data()), t.size()) !=
        UsbHandle::Result::OK)
        return;
    gFill ^= 1;
    gTx[gFill].Clear();
}

// USB IRQ: queue the bytes; on overflow they are dropped and the parser resyncs at the next F0.
void onMidiRx(uint8_t* data, size_t size, void*) {
    for (size_t i = 0; i < size; i++) gRx.Push(data[i]);
}

uint8_t* romSlotRam(int slot) {
    return slot < 3 ? gRomSbc + slot * 0x800 : gRomNvs + (slot - 3) * 0x1000;
}

uint32_t romSlotAddr(int slot) { return kRomFlashBase + uint32_t(slot) * 0x1000; }

// The slot's memory-mapped flash, with stale cache lines dropped.
const uint8_t* romSlotFlash(int slot) {
    uint8_t* p = static_cast<uint8_t*>(hw.seed.qspi.GetData(romSlotAddr(slot)));
    dsy_dma_invalidate_cache_for_buffer(p, barc::RomSize(slot));
    return p;
}

// Builds the RAM image, per slot: the flashed copy if its byte sum matches the socket
// (an erased sector sums to 0, never a socket index), else the built-in image, else
// FF fill (empty). Main loop, engine stopped.
void loadRomImage() {
    char map[barc::kRomSlots + 1];
    for (int s = 0; s < barc::kRomSlots; s++) {
        const uint16_t size = barc::RomSize(s);
        uint8_t* dst = romSlotRam(s);
        const uint8_t* flash = romSlotFlash(s);
        if (barc::RomByteSum(flash, size) == barc::RomSocket(s)) {
            std::memcpy(dst, flash, size);
            gRomState[s] = barc::kRomFlash;
        } else {
#if LEX_EMBED_ROMS
            std::memcpy(dst, s < 3 ? lex_rom_sbc + s * 0x800 : lex_rom_nvs + (s - 3) * 0x1000, size);
            gRomState[s] = barc::kRomBuiltIn;
#else
            std::memset(dst, 0xFF, size);
            gRomState[s] = barc::kRomEmpty;
#endif
        }
        gRomCrc[s] = gRomState[s] == barc::kRomEmpty ? 0 : barc::Crc32(dst, size);
        map[s] = "-FB"[gRomState[s]];
    }
    map[barc::kRomSlots] = '\0';
    logLine("roms %s (F flash, B built-in, - empty)", map);
    gRomsDirty = true;
}

// Lists the programs of the RAM image; false if a slot is empty or it lists none.
bool scanRoms() {
    gPrograms.count = 0;
    for (uint8_t s : gRomState)
        if (s == barc::kRomEmpty) return false;
    listPrograms({gRomSbc, gRomNvs}, gPrograms);
    return gPrograms.count > 0;
}

// Writes `img` to the slot's sector and reads it back (libDaisy's Write reports no
// page errors). Main loop; the engine may run, it reads only the RAM image.
uint8_t writeRomSlot(int slot, const uint8_t* img) {
    const uint32_t a = romSlotAddr(slot);
    const uint16_t size = barc::RomSize(slot);
    // Write only reads the buffer; libDaisy's API is not const-correct.
    if (hw.seed.qspi.Erase(a, a + 0x1000) != QSPIHandle::Result::OK ||
        hw.seed.qspi.Write(a, size, const_cast<uint8_t*>(img)) != QSPIHandle::Result::OK)
        return barc::kRomErrFlash;
    return std::memcmp(romSlotFlash(slot), img, size) == 0 ? barc::kRomOk : barc::kRomErrVerify;
}

uint8_t eraseRomSlot(int slot) {
    const uint32_t a = romSlotAddr(slot);
    return hw.seed.qspi.Erase(a, a + 0x1000) == QSPIHandle::Result::OK ? barc::kRomOk
                                                                       : barc::kRomErrFlash;
}

// Queues ROMS if it changed or was asked for; retried next pass if the buffer is full.
void sendRoms() {
    if (gRomsDirty && gTx[gFill].AppendRoms(LEX_EMBED_ROMS, gRomState, gRomCrc)) gRomsDirty = false;
}

// Mono output (mono-in/mono-out). These programs write DAC A with D and B with C
// (emu/README.md "Presets"): only two distinct signals, with the voices split
// between A and C, so the mono output is the mean of A and C. The Lexicon 224
// manual gives the same A + C rule for its comparable programs (plates, chorus);
// V8.2 has no such table, so this list follows the A=D, B=C microcode routing.
// Exception: CD PLATE A (0x21) is A=D, B=C too, but its C is largely A inverted and
// 11.7 ms later (r = -0.90), so their mean is comb-filtered (a -15.6 dB notch).
// It uses A alone, as the 224 manual does for CD Plate A.
// PLATE/CHORUS (0x22) has four distinct outputs and plays their mean (0x0F), a user
// choice. On guitar its four outputs lie within 0.9 dB of each other and no pair
// correlates beyond |r| = 0.36, so the mean does not comb-filter (ripple -3.3 ... +3.6 dB).
// The other programs have four distinct outputs and use A alone.
// The editor can override this per program, see gOutOverrides. Its MONO OUTPUT panel
// (editor/src/components/MonoOut.tsx) names these programs: keep both lists in step.
constexpr uint8_t kSumAC[] = {0x40, 0x02, 0x03, 0x10, 0x0C, 0x14, 0x24, 0x11};
constexpr uint8_t kPlateChorus = 0x22;

// The program's own outMix: A+C (0x05) for kSumAC, A+B+C+D (0x0F) for PLATE/CHORUS,
// else A (0x01).
uint8_t defaultOutMix(uint8_t id) {
    if (id == kPlateChorus) return 0x0F;
    for (uint8_t s : kSumAC)
        if (s == id) return 0x05;
    return 0x01;
}

const char* outMixText(uint8_t m) {
    switch (m) {
    case 0x01: return "A";
    case 0x04: return "C";
    case 0x05: return "A+C";
    case 0x0F: return "A+B+C+D";
    default: return "?";
    }
}

// Main loop: the editor's override for the program, else its default.
uint8_t effectiveOutMix(uint8_t id) {
    return gOutOverrides.mix[id] ? gOutOverrides.mix[id] : defaultOutMix(id);
}

// Reads the overrides from flash; an erased or corrupt sector leaves them all default.
void loadOutMixStore() {
    uint8_t* p = static_cast<uint8_t*>(hw.seed.qspi.GetData(kOutMixFlashAddr));
    dsy_dma_invalidate_cache_for_buffer(p, OutMixTable::kRecordSize);
    if (!gOutOverrides.Load(p)) {
        logLine("outmix store: empty");
        return;
    }
    int n = 0;
    for (uint8_t m : gOutOverrides.mix) n += m != 0;
    logLine("outmix store: %d overrides", n);
}

// Writes the overrides to their sector and reads them back, like writeRomSlot.
// Main loop; blocks it for the sector erase.
bool saveOutMixStore() {
    uint8_t rec[OutMixTable::kRecordSize];
    gOutOverrides.Save(rec);
    const uint32_t a = kOutMixFlashAddr;
    if (hw.seed.qspi.Erase(a, a + 0x1000) != QSPIHandle::Result::OK ||
        hw.seed.qspi.Write(a, sizeof rec, rec) != QSPIHandle::Result::OK)
        return false;
    uint8_t* p = static_cast<uint8_t*>(hw.seed.qspi.GetData(a));
    dsy_dma_invalidate_cache_for_buffer(p, sizeof rec);
    return std::memcmp(p, rec, sizeof rec) == 0;
}

// OUTMIX from the editor (main loop): stores `mix` (0 = default) for the loaded
// program, saves the table and hands the effective mix to the callback. A choice
// equal to the default is stored as given; only DEFAULT clears an override.
void setOutOverride(uint8_t mix) {
    if (gState == NoRoms || gPrograms.count == 0) return;
    const Program& p = gPrograms.items[gProgIndex];
    if (gOutOverrides.mix[p.id] == mix) return;
    gOutOverrides.mix[p.id] = mix;
    gOutMixWanted = effectiveOutMix(p.id);
    const bool saved = saveOutMixStore();
    logLine("%s out %s%s, %s", p.name, outMixText(effectiveOutMix(p.id)), mix ? "" : " (default)",
            saved ? "saved" : "not saved (flash error)");
}

// Wet makeup gain (dB) that brings each program's wet output to the loudness of the
// dry input: BS.1770 integrated loudness of guitar.wav (left) through this pedal's
// engine config at factory slider settings, input at -12 dBFS peak. The same within
// 0.15 dB at -24 dBFS and within 0.4 dB at 0 dBFS, except RES CHORDS (0.9 dB, it
// saturates at full-scale input). Knob changes (decay above all) move the wet level.
struct WetTrim {
    uint8_t id;
    float db;
};
constexpr WetTrim kWetTrim[] = {
    {0x01, 3.1f}, {0x20, 3.5f}, {0x05, 4.4f}, {0x04, 4.8f}, {0x40, 8.9f}, {0x08, 3.5f},
    {0x80, 4.3f}, {0x02, 6.4f}, {0x03, 5.0f}, {0x21, 0.3f}, {0x10, 1.9f}, {0x0C, 4.4f},
    {0x14, -6.5f}, {0x24, 8.6f}, {0x06, 5.1f}, {0x0A, 4.4f}, {0x12, 5.8f}, {0x22, 8.8f},
    {0x11, 7.5f}, {0x41, 4.3f}, {0x81, 4.3f}, {0x42, 3.1f}};

float wetTrimDb(uint8_t id) {
    for (const WetTrim& t : kWetTrim)
        if (t.id == id) return t.db;
    return 0.0f;
}

// "+3.1" / "-6.5" for the log (newlib-nano printf has no %f).
struct Db {
    char s[16];
    explicit Db(float db) {
        const int t = int(std::lround(db * 10.0f));
        snprintf(s, sizeof s, "%c%d.%d", t < 0 ? '-' : '+', std::abs(t) / 10, std::abs(t) % 10);
    }
};

// Levers: switch 1 MODE ENH, switch 2 DECAY OPT; switches 3/4 the bank.
uint8_t leverOptions() {
    return uint8_t((hw.switches[Terrarium::SWITCH_1].Pressed() ? Machine::kOptModeEnh : 0) |
                   (hw.switches[Terrarium::SWITCH_2].Pressed() ? Machine::kOptDecayOpt : 0));
}

int leverBank() {
    return bankFromLevers(hw.switches[Terrarium::SWITCH_3].Pressed(),
                          hw.switches[Terrarium::SWITCH_4].Pressed());
}

void startProgram(int i) {
    gState = Loading;
    const uint32_t t0 = System::GetNow();
    const Program& p = gPrograms.items[i];
    const uint8_t out = effectiveOutMix(p.id);
    EngineConfig cfg{};
    cfg.roms = {gRomSbc, gRomNvs};
    cfg.dmem = gDmem;
    cfg.kernel = gKernel;
    cfg.sampleRate = kSampleRate;
    cfg.programId = p.id;
    cfg.numInputs = 1;
    cfg.numOutputs = 1;
    cfg.outMix[0] = gBootOutMix = out;  // DAC bits: A = 1, B = 2, C = 4, D = 8
    gOutMixWanted = out;
    const float trimDb = wetTrimDb(p.id);
    gWetTrim = std::pow(10.0f, trimDb / 20.0f);
    const char* err = nullptr;
    const bool ok = gEngine.start(cfg, &err);
    if (ok)
        logLine("program %s (id %02X) loop %d steps, out %s%s, latency %d, boot %u ms, wet %s dB",
                p.name, p.id, gEngine.loopLen(), outMixText(out),
                gOutOverrides.mix[p.id] ? " (editor)" : "", gEngine.latency(),
                unsigned(System::GetNow() - t0), Db(trimDb).s);
    else
        logLine("engine start failed: %s", err);
    if (ok) gFlashStartMs = System::GetNow();
    gPageMask = 0;
    for (int pg = 1; ok && pg <= 12; pg++)
        if (gEngine.hasPage(pg)) gPageMask |= uint16_t(1u << (pg - 1));
    gState = ok ? Running : Failed;
}

void parkKnobs(const float* pos) {
    gKnobs.Park(pos);
    for (int& s : gLastSent) s = -1;
}

void parkKnob(int k, const float* pos) {
    gKnobs.ParkOne(k, pos[k]);
    gLastSent[k] = -1;
}

// Knob, lever, footswitch and editor command handling. Engine calls only while
// Running: the main loop owns the engine in every other state.
void processControls(int state, bool fs2Advance, int layer) {
    float pos[kKnobs];
    for (int k = 0; k < kKnobs; k++) pos[k] = hw.knob[kKnobIndex[k]].Value();

    if (state == Running && gPrevState != Running) {  // a program just started
        parkKnobs(pos);
        gLayer = layer;
        gAppliedOptions = uint8_t(gEngine.options() & kOptionBits);  // the preset's bits
        gOptions.Loaded(gAppliedOptions);
        gAppliedOutMix = gBootOutMix;
        if (!gMixPrimed) {
            gMix = pos[0];
            gMixPrimed = true;
        }
    }
    if (state != Running && gPrevState == Running) gOptions.BeginLoad();
    gPrevState = state;
    if (layer != gLayer) {
        parkKnobs(pos);
        gLayer = layer;
    }

    // Editor commands. A slider or the mix moved by the editor re-parks the knob
    // that drives it, so the knob takes over again only once it is moved.
    barc::Command c;
    while (gCommands.Pop(c)) {
        switch (c.cmd) {
            case barc::kSlider: {
                if (state != Running || !gEngine.hasPage(c.a)) break;
                gEngine.moveSlider(c.a, c.b, uint8_t(c.value));  // false: queue full, dropped
                const int k = knobFor(gLayer, c.a, c.b);
                if (k >= 0) parkKnob(k, pos);
                break;
            }
            case barc::kOption:
                gOptions.Set(c.a, c.b != 0);
                break;
            case barc::kMix: {
                gMix = c.value / float(barc::kMixMax);
                gMixPrimed = true;
                const int k = knobFor(gLayer, 0, 0);
                if (k >= 0) parkKnob(k, pos);
                break;
            }
            case barc::kBypass:
                gBypass = c.b != 0;
                break;
        }
    }

    for (int k = 0; k < kKnobs; k++) {
        if (!gKnobs.Update(k, pos[k])) continue;
        if (layer == 1) gFs2.MarkEdited();
        const KnobTarget t = kKnobMap[layer][k];
        if (t.page == 0) {
            gMix = pos[k];
            continue;
        }
        if (state != Running) continue;
        const uint8_t p = sliderPosition(pos[k]);
        if (!sliderChanged(gLastSent[k], p)) continue;
        // Absent page: nothing to move. False: queue full, retried next block.
        bool ok = !gEngine.hasPage(t.page) || gEngine.moveSlider(t.page, t.slider, p);
        if (t.page == 1 && gEngine.hasPage(2)) ok = gEngine.moveSlider(2, t.slider, p) && ok;
        if (ok) gLastSent[k] = p;
    }

    const uint8_t desired = gOptions.Update(gLeverOptions);
    if (state == Running) {
        for (uint8_t bit : {Machine::kOptDynDecay, Machine::kOptModeEnh, Machine::kOptDecayOpt})
            if (((desired ^ gAppliedOptions) & bit) && gEngine.setOption(bit, desired & bit))
                gAppliedOptions ^= bit;
    }
    if (state == Running) {
        const uint8_t out = gOutMixWanted.load(std::memory_order_relaxed);
        if (out != gAppliedOutMix && gEngine.setOutMix(0, out)) gAppliedOutMix = out;
    }

    // A settled lever change loads its bank's first program once, as an edge, so a
    // program the editor picked in another bank stays until the levers move.
    const int settled = gBankSel.Update(leverBank());
    if (state == Running && gLoadRequest < 0) {
        if (gBankSel.TakeChange())
            gLoadRequest = settled * kBankSize;
        else if (fs2Advance)
            gLoadRequest = nextInBank(gProgIndex, gPrograms.count);
    }
}

// Fills gSnap if the main loop asked for it. Called before the StopRequested ->
// Stopped transition, so a Running `state` still means this callback owns the engine.
void takeSnapshot(int state) {
    if (gSnapPhase.load(std::memory_order_acquire) != 1) return;
    gSnap.running = state == Running;
    if (gSnap.running) {
        gEngine.sliderValues(gSnap.sliders);
        gSnap.options = uint8_t(gEngine.options() & kOptionBits);
    } else {
        gSnap.options = gOptions.Desired();
    }
    gSnap.mix = uint16_t(std::lround(std::clamp(gMix, 0.0f, 1.0f) * barc::kMixMax));
    gSnapPhase.store(2, std::memory_order_release);
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    gLoad.OnBlockStart();
    hw.ProcessAnalogControls();
    hw.ProcessDigitalControls();

    if (hw.switches[Terrarium::FOOTSWITCH_1].RisingEdge()) gBypass = !gBypass;
    gLeverOptions = leverOptions();
    const Switch& fs2 = hw.switches[Terrarium::FOOTSWITCH_2];
    const bool advance = gFs2.Update(fs2.Pressed(), fs2.FallingEdge());
    const int st = gState;
    processControls(st, advance, gFs2.Held() ? 1 : 0);
    takeSnapshot(st);
    if (gState == StopRequested) gState = Stopped;

    float* wetPtr[1] = {gWet};
    if (gState == Running) {
        if (!gEngine.process(&in[0], wetPtr, size)) {
            gState = Failed;
            for (size_t i = 0; i < size; i++) gWet[i] = 0.0f;
        }
    } else {
        for (size_t i = 0; i < size; i++) gWet[i] = 0.0f;
    }

    // The wet trim rides on the per-block wet gain: no extra per-sample work.
    float dryGain, wetGain;
    mixGains(gMix, dryGain, wetGain);
    wetGain *= gWetTrim.load(std::memory_order_relaxed);
    const bool bypass = gBypass;
    for (size_t i = 0; i < size; i++) {
        out[0][i] = bypass ? in[0][i] : in[0][i] * dryGain + gWet[i] * wetGain;
        out[1][i] = out[0][i];
    }

    const int state = gState;
    const uint32_t now = System::GetNow();
    gLed1.Set(bypass ? 0.0f : 1.0f);
    gLed2.Set(state == Running  ? (flashLit(gProgIndex % kBankSize + 1, now - gFlashStartMs) ? 1.0f : 0.0f)
              : state == Failed ? float((now / 125) & 1)
              : state == NoRoms ? float((now / 500) & 1)
                                : 1.0f);
    gLed1.Update();
    gLed2.Update();
    gLoad.OnBlockEnd();
    crash::gTrace.program = gProgIndex;
    crash::gTrace.state = state;
    crash::gTrace.nowMs = now;
    crash::gTrace.blocks = crash::gTrace.blocks + 1;
}

// Main loop: the editor's view of program `index` in `state`, with `s`'s options and mix.
barc::StateMsg stateMsg(int index, int state, const Snapshot& s) {
    return {uint8_t(index),
            uint8_t(gPrograms.count),
            state == NoRoms ? uint8_t(0) : gPrograms.items[index].id,
            uint8_t(state == Running ? 1 : state == Failed ? 2 : state == NoRoms ? 3 : 0),
            gBypass.load(),
            s.options,
            s.mix,
            gPageMask,
            state == NoRoms ? uint8_t(0) : gOutOverrides.mix[gPrograms.items[index].id],
            state == NoRoms ? uint8_t(0) : defaultOutMix(gPrograms.items[index].id)};
}

// Main loop, after a new snapshot: queue STATE if it changed and PAGE for each present
// page whose registers changed (everything when gForce). What does not fit in the
// send buffer stays unsent and is retried at the next snapshot.
void sendUpdates() {
    const barc::StateMsg s = stateMsg(gProgIndex, gState, gLatest);
    if ((gForce || s != gSentState) && gTx[gFill].AppendState(s)) gSentState = s;
    for (int p = 1; gLatest.running && p <= 12; p++) {
        if (!(gPageMask >> (p - 1) & 1)) continue;
        const uint8_t* v = gLatest.sliders[p - 1];
        if (!gForce && gSentValid[p - 1] && std::memcmp(v, gSent[p - 1], 6) == 0) continue;
        if (!gTx[gFill].AppendPage(p, v)) continue;
        std::memcpy(gSent[p - 1], v, 6);
        gSentValid[p - 1] = true;
    }
    gForce = false;
}

// Loads program `r` (< gPrograms.count), blocking this loop for the boot (0.8 s).
void loadProgram(int r) {
    // Tell the editor before the boot blocks this loop.
    gPageMask = 0;
    gTx[gFill].AppendState(stateMsg(r, Loading, gLatest));
    flushTx();
    gState = StopRequested;
    while (gState != Stopped) {
    }
    gProgIndex = r;
    startProgram(r);
    // A snapshot taken before or during the load is stale.
    if (gSnapPhase.load(std::memory_order_acquire) == 2) gSnapPhase.store(0);
    for (bool& v : gSentValid) v = false;
    gLatest.running = false;
    gForce = true;
}

// After a ROM flash or erase: rebuilds the RAM image with the engine stopped, then
// reloads the current program, or waits in NoRoms while a slot is empty.
void reloadRoms() {
    gState = StopRequested;
    while (gState != Stopped) {
    }
    loadRomImage();
    sendRoms();  // with the ROM_RESULT, before the boot blocks this loop
    if (scanRoms()) {
        loadProgram(gProgIndex < gPrograms.count ? gProgIndex.load() : 0);
    } else {
        gPageMask = 0;
        gState = NoRoms;
        gForce = true;
    }
}

const char* eventKindText(uint32_t kind) {
    switch (kind) {
    case crash::kFault: return "cpu fault";
    case crash::kStall: return "audio stall";
    case crash::kReset: return "unexplained reset";
    default: return "none";
    }
}

// Line `part` (0-2) of the last lockup event (crash_trace.h).
void logEvent(int part) {
    const volatile crash::Event& e = crash::gEvent;
    if (part == 0)
        logLine("event %lu: %s at %lu s, prog %ld state %ld blocks %lu, rsr %08lX", (unsigned long)e.count,
                eventKindText(e.kind), (unsigned long)(e.trace.nowMs / 1000), (long)e.trace.program,
                (long)e.trace.state, (unsigned long)e.trace.blocks, (unsigned long)e.rsr);
    else if (part == 1)
        logLine("  pc %08lX lr %08lX xpsr %08lX cfsr %08lX hfsr %08lX mmfar %08lX bfar %08lX exc %08lX",
                (unsigned long)e.pc, (unsigned long)e.lr, (unsigned long)e.xpsr, (unsigned long)e.cfsr,
                (unsigned long)e.hfsr, (unsigned long)e.mmfar, (unsigned long)e.bfar, (unsigned long)e.excReturn);
    else
        logLine("  dma lisr %08lX s0 %08lX/%lu s1 %08lX/%lu sai %08lX %08lX", (unsigned long)e.dmaLisr,
                (unsigned long)e.dma0Cr, (unsigned long)e.dma0Ndtr, (unsigned long)e.dma1Cr,
                (unsigned long)e.dma1Ndtr, (unsigned long)e.saiASr, (unsigned long)e.saiBSr);
}

}  // namespace

int main() {
    __set_FPSCR(__get_FPSCR() | (1u << 24));  // FZ: flush denormals to zero
    hw.Init(true);                             // 480 MHz
    configureMpu();
    crash::Init();
    hw.SetAudioBlockSize(kBlockSize);
    gLed1.Init(hw.seed.GetPin(Terrarium::LED_1), false);
    gLed2.Init(hw.seed.GetPin(Terrarium::LED_2), false);

    MidiUsbTransport::Config midiCfg;
    midiCfg.periph = MidiUsbTransport::Config::INTERNAL;
    gMidi.Init(midiCfg);  // the Seed's own USB port, as a USB MIDI device
    gMidi.StartRx(onMidiRx, nullptr);
    logLine("lex224 sysclk %u MHz", unsigned(System::GetSysClkFreq() / 1000000));

    initKernelTable(gKernel);
    loadRomImage();
    loadOutMixStore();
    gLoad.Init(hw.AudioSampleRate(), hw.AudioBlockSize());

    // Fill the switch debouncers (one shift per ms) before reading the levers.
    for (int i = 0; i < 10; i++) {
        hw.ProcessDigitalControls();
        System::Delay(1);
    }
    gLeverOptions = leverOptions();
    gOptions.Init(gLeverOptions);
    const int bank = leverBank();
    gBankSel.Init(bank);
    gProgIndex = bank * kBankSize;
    if (scanRoms()) {
        if (gProgIndex >= gPrograms.count) gProgIndex = 0;
        startProgram(gProgIndex);
    } else {
        gState = NoRoms;
    }
    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    // Lockup recovery (crash_trace.h). The longest main-loop block is a program load (0.8 s);
    // ROM commands are kicked one by one below.
    crash::StartWatchdog(4000);
    constexpr uint32_t kStallMs = 100;
    uint32_t lastLog = System::GetNow(), lastBlocks = crash::gTrace.blocks, lastBlocksMs = lastLog;
    unsigned logTick = 0;
    for (;;) {
        crash::KickWatchdog();
        // libDaisy stops receiving when its ring overflows.
        if (!gMidi.RxActive()) {
            gMidi.FlushRx();
            gMidi.StartRx(onMidiRx, nullptr);
        }
        int select = -1;
        bool romReload = false;
        uint8_t byte;
        barc::Command c;
        while (gRx.Pop(byte)) {
            if (!gParser.Feed(byte, c)) continue;
            crash::KickWatchdog();
            switch (c.cmd) {
                case barc::kHello:
                    gForce = true;
                    gRomsDirty = true;
                    break;
                case barc::kProgram:
                    if (c.a < gPrograms.count) select = c.a;
                    break;
                // ROM commands block this loop for flash work (erase ~50 ms per sector).
                case barc::kRomBegin:
                    gUpload.Begin(c.a);
                    gTx[gFill].AppendRomAck(c.a, 0);
                    break;
                case barc::kRomData: {
                    uint16_t next = 0;
                    if (gUpload.Data(c.a, c.value, gParser.Chunk(), next))
                        gTx[gFill].AppendRomAck(c.a, next);
                    else
                        gTx[gFill].AppendRomResult(c.a, barc::kRomErrSequence);
                    break;
                }
                case barc::kRomCommit: {
                    uint8_t code = gUpload.Commit(c.a, c.crc);
                    if (code == barc::kRomOk) code = writeRomSlot(c.a, gUpload.Image());
                    gTx[gFill].AppendRomResult(c.a, code);
                    romReload |= code == barc::kRomOk;
                    break;
                }
                case barc::kRomErase: {
                    const uint8_t code = eraseRomSlot(c.a);
                    gTx[gFill].AppendRomResult(c.a, code);
                    romReload |= code == barc::kRomOk;
                    break;
                }
                // Writes flash too (a sector erase), so it stays in this loop.
                case barc::kOutMix:
                    setOutOverride(c.a);
                    break;
                default:
                    gCommands.Push(c);  // dropped if full
            }
        }
        if (romReload) reloadRoms();

        int r = gLoadRequest.exchange(-1);
        if (select >= 0) r = select;  // the editor wins over a lever or FS2 request
        if (r >= 0 && r < gPrograms.count) loadProgram(r);
        sendRoms();

        const uint32_t now = System::GetNow();
        const uint32_t blocks = crash::gTrace.blocks;
        if (blocks != lastBlocks) {
            lastBlocks = blocks;
            lastBlocksMs = now;
        } else if (now - lastBlocksMs > kStallMs) {
            crash::StallReset();
        }
        if (gSnapPhase.load(std::memory_order_acquire) == 0 && now - gLastSnapReq >= 20) {
            gLastSnapReq = now;
            gSnapPhase.store(1, std::memory_order_release);
        }
        if (gSnapPhase.load(std::memory_order_acquire) == 2) {
            gLatest = gSnap;
            gSnapPhase.store(0, std::memory_order_release);
            sendUpdates();
        }
        if (now - lastLog >= 1000) {
            lastLog = now;
            // Once a restart has been recorded, its three event lines take turns with the load line.
            const unsigned eventLines = crash::gEvent.count > 0 ? 3 : 0;
            const unsigned part = logTick++ % (1 + eventLines);
            if (part > 0)
                logEvent(int(part) - 1);
            else if (gState == Running)
                logLine("%s load avg %d max %d permille", gPrograms.items[gProgIndex].name,
                        int(gLoad.GetAvgCpuLoad() * 1000), int(gLoad.GetMaxCpuLoad() * 1000));
            gLoad.Reset();
            gForce = true;
        }
        flushTx();
    }
}
