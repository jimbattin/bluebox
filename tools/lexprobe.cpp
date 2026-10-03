// lexprobe: research probes on the emulated 224XL. Each command boots one
// factory program (optionally with patched ROM bytes) in the lex224emu machine
// and reports firmware or DSP state. Usage and examples: tools/README.md.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "audio.h"
#include "machine.h"
#include "romfile.h"

namespace {

const char* const kUsage =
    "usage: lexprobe [--rom-dir DIR] COMMAND ID [ADDR=VAL ...] [ARGS]\n"
    "  ID        factory program id, hex (e.g. 01 = CONCERT HALL; see lex224emu --list)\n"
    "  ADDR=VAL  patch a ROM byte before boot (hex; 0000-17FF SBC, 8000-FFFF NVS)\n"
    "commands:\n"
    "  steps ID                  every WCS step after boot: template and loaded fields\n"
    "  mem ID ADDR[:LEN] ...     hex dump of 8080 memory after boot (LEN hex, default 10)\n"
    "  groups ID                 the step-group table at 3CF4 and its entry bytes\n"
    "  diff ID BLOCK [DELTA]     add DELTA (default 8) to each preset-block byte in turn,\n"
    "                            boot, and list the WCS steps and RAM bytes that change\n"
    "  handlers ID [PASS]        builder pass PASS (default 1): each slot handler entry\n"
    "                            (table A91E) and the WCS writes it performs\n"
    "  access ID LO[-HI] [SEC]   8080 reads/writes of LO..HI by pc, during boot and\n"
    "                            SEC seconds (default 0) of silent audio\n"
    "  run ID BURST TOTAL OUT.wav [--amp X] [--burst2 START,LEN] [--poke ADDR=VAL@SEC]\n"
    "      [--trace STEP] [--pc ADDR] [--watch ADDR]\n"
    "                            noise burst of BURST s (amplitude X of full scale,\n"
    "                            default 0.3) then silence, TOTAL s at the DSP rate;\n"
    "                            writes the 4 DACs to OUT.wav and reports WCS writes,\n"
    "                            X-register reads and the requested traces\n";

[[noreturn]] void usageExit() {
    std::fputs(kUsage, stderr);
    std::exit(2);
}

unsigned hexArg(const std::string& s) {
    size_t used = 0;
    const unsigned long v = std::stoul(s, &used, 16);
    if (used != s.size()) throw std::invalid_argument(s);
    return unsigned(v);
}

uint8_t gSbc[0x1800], gNvs[0x8000];
int16_t gDmem[65536];

struct Silence : SampleIo {
    bool nextInput(int16_t& l, int16_t& r) override {
        l = r = 0;
        return true;
    }
    void frame(const DacFrame&) override {}
};

// Machine with access hooks. Commands set the hooks they need.
struct Probe : Machine {
    bool audio = false;  // set after boot()
    // Hooks: onFetch for instruction-stream bytes (opcode and operands, seen
    // as a + 1 == pc), onRead for data reads, onWrite before a write lands.
    virtual void onFetch(uint16_t) {}
    virtual void onRead(uint16_t, uint8_t) {}
    virtual void onWrite(uint16_t, uint8_t) {}
    virtual void onIn(uint8_t) {}

    uint8_t read(uint16_t a) override {
        const uint8_t v = Machine::read(a);
        if (uint16_t(a + 1) == cpu.pc)
            onFetch(a);
        else
            onRead(a, v);
        return v;
    }
    void write(uint16_t a, uint8_t v) override {
        onWrite(a, v);
        Machine::write(a, v);
    }
    uint8_t in(uint8_t p) override {
        onIn(p);
        return Machine::in(p);
    }

    // Side-effect-free 8080 view (no WCS stall).
    uint8_t peek(uint16_t a) const {
        if (a < 0x1800) return roms_.sbc[a];
        if (a >= 0x2000 && a < 0x2800) return nvsRam[a - 0x2000];
        if (a >= 0x3C00 && a < 0x4000) return sbcRam[a - 0x3C00];
        if (a >= 0x4000 && a < 0x4200) return dsp.wcs[a - 0x4000];
        if (a >= 0x8000) return roms_.nvs[a - 0x8000];
        return 0xFF;
    }
    uint16_t peek16(uint16_t a) const { return uint16_t(peek(a) | peek(uint16_t(a + 1)) << 8); }
    void poke(uint16_t a, uint8_t v) {
        if (a >= 0x2000 && a < 0x2800) nvsRam[a - 0x2000] = v;
        if (a >= 0x3C00 && a < 0x4000) sbcRam[a - 0x3C00] = v;
    }
    uint32_t wcsWord(int n) const {
        const uint8_t* s = dsp.wcs + 4 * n;
        return uint32_t(s[0] | s[1] << 8 | s[2] << 16) | uint32_t(s[3]) << 24;
    }
    const uint8_t* wcs() const { return dsp.wcs; }
    const uint8_t* sbcRamBytes() const { return sbcRam; }
    const uint8_t* nvsRamBytes() const { return nvsRam; }
};

struct Args {
    uint8_t id = 0;
    std::vector<std::string> pos;                 // command arguments after ID
    std::vector<std::pair<unsigned, uint8_t>> patches;
    std::map<std::string, std::vector<std::string>> opts;  // --name VALUE
};

void applyPatches(const Args& a, uint8_t* sbc, uint8_t* nvs) {
    for (auto [addr, v] : a.patches) {
        if (addr < 0x1800)
            sbc[addr] = v;
        else if (addr >= 0x8000 && addr <= 0xFFFF)
            nvs[addr - 0x8000] = v;
        else
            throw std::invalid_argument("patch address outside ROM: " + std::to_string(addr));
    }
}

void bootOrDie(Probe& m, const uint8_t* sbc, const uint8_t* nvs, uint8_t id) {
    m.reset({sbc, nvs}, gDmem);
    const char* e = nullptr;
    if (!m.boot(id, &e)) {
        std::printf("boot failed: %s\n", e);
        std::exit(1);
    }
    m.audio = true;
}

const char* opName(unsigned op) { return op == 0 ? "MR" : op == 1 ? "MW" : op == 2 ? "IO" : "--"; }

// ---- steps --------------------------------------------------------------
int cmdSteps(const Args& a) {
    Probe m;
    bootOrDie(m, gSbc, gNvs, a.id);
    std::printf("loop %d record %04X\n", m.loopLen, m.recordPtr);
    std::printf("# n pc run op template wcs m sign xfer zero prot [io: src dac wrX reset]\n");
    static const char* const src[] = {"RDAD", "RDXREG", "RDRREG", "-"};
    for (int n = 127; n >= 0; n--) {
        const int pc = 127 - n;
        const uint16_t t = uint16_t(m.recordPtr + 0xAA + 4 * n);
        const unsigned op = m.peek(uint16_t(t + 2)) & 3;
        const uint32_t w = m.wcsWord(n);
        std::printf("%3d %3d %d %s %04X %04X %2u %c %d %d %d", n, pc, pc < m.loopLen, opName(op),
                    m.peek16(t), unsigned(w & 0xFFFF), unsigned(~w >> 26) & 63u,
                    (w >> 23) & 1 ? '+' : '-', !((w >> 24) & 1), !((w >> 25) & 1), int((w >> 22) & 1));
        if (op == 2) {
            const unsigned low = w & 0xFFFF;
            char dac[5] = "....";
            if (!(low & 0x80)) {
                if (!(low & 0x800)) dac[0] = 'A';
                if (!(low & 0x400)) dac[1] = 'B';
                if (!(low & 0x200)) dac[2] = 'C';
                if (!(low & 0x100)) dac[3] = 'D';
            }
            std::printf("  %s %s %d %d", src[(low >> 12) & 3], dac, !(low & 0x40), !(low & 0x08));
        }
        std::printf("\n");
    }
    return 0;
}

// ---- mem ----------------------------------------------------------------
int cmdMem(const Args& a) {
    if (a.pos.empty()) usageExit();
    Probe m;
    bootOrDie(m, gSbc, gNvs, a.id);
    for (const std::string& s : a.pos) {
        const auto c = s.find(':');
        const unsigned lo = hexArg(s.substr(0, c));
        const unsigned len = c == std::string::npos ? 0x10 : hexArg(s.substr(c + 1));
        std::printf("%04X:", lo);
        for (unsigned i = 0; i < len; i++) std::printf(" %02X", m.peek(uint16_t(lo + i)));
        std::printf("\n");
    }
    return 0;
}

// ---- groups -------------------------------------------------------------
// Table at 3CF4 (NVS3 AA81-AB38): [entry pointer lo][hi][header] per group;
// header low nibble = entry count, bits 5-4 = type, bits 7-6 = flags.
int cmdGroups(const Args& a) {
    Probe m;
    bootOrDie(m, gSbc, gNvs, a.id);
    std::printf("record %04X\n", m.recordPtr);
    int last = -1;
    for (int k = 0; k < 85; k++)
        if (m.peek16(uint16_t(0x3CF4 + 3 * k))) last = k;
    for (int k = 0; k <= last; k++) {
        const uint16_t d = uint16_t(0x3CF4 + 3 * k);
        const uint16_t p = m.peek16(d);
        const uint8_t h = m.peek(uint16_t(d + 2));
        const unsigned type = (h >> 4) & 3, count = h & 15;
        // An entry list ends where the next group's starts; the last one is
        // sized from its count (type 1 pairs, 6 bytes, are then undercounted).
        unsigned len = count * (type == 2 ? 5u : 3u);
        if (k < last) len = unsigned(m.peek16(uint16_t(d + 3)) - p);
        std::printf("g%-2d hdr %02X type %u n %2u flags %u%u ptr %04X:", k, h, type, count, h >> 7,
                    (h >> 6) & 1, p);
        for (unsigned i = 0; i < len && i < 64; i++) std::printf(" %02X", m.peek(uint16_t(p + i)));
        if (type != 1 && count) {  // fixed-size entries: decode the WCS steps
            std::printf("  steps");
            const unsigned size = type == 2 ? 5 : 3;
            for (unsigned e = 0; e < count; e++)
                std::printf(" %d", int(m.peek16(uint16_t(p + size * e)) - 0x4003) / 4);
        }
        std::printf("\n");
    }
    return 0;
}

// ---- diff ---------------------------------------------------------------
struct Snap {
    uint8_t wcs[512], nvsRam[0x800], sbcRam[0x400];
    int loop;
};

bool snapBoot(const uint8_t* nvs, uint8_t id, Snap& s) {
    static Probe m;
    m.reset({gSbc, nvs}, gDmem);
    const char* e = nullptr;
    if (!m.boot(id, &e)) return false;
    std::memcpy(s.wcs, m.wcs(), sizeof s.wcs);
    std::memcpy(s.nvsRam, m.nvsRamBytes(), sizeof s.nvsRam);
    std::memcpy(s.sbcRam, m.sbcRamBytes(), sizeof s.sbcRam);
    s.loop = m.loopLen;
    return true;
}

int cmdDiff(const Args& a) {
    if (a.pos.empty() || a.pos.size() > 2) usageExit();
    const unsigned blk = hexArg(a.pos[0]);
    const int delta = a.pos.size() > 1 ? std::stoi(a.pos[1]) : 8;
    if (blk < 0x8000 || blk > 0xFFF0) usageExit();
    static uint8_t patched[0x8000];
    Snap base, p;
    if (!snapBoot(gNvs, a.id, base)) {
        std::printf("boot failed\n");
        return 1;
    }
    const unsigned len = gNvs[blk + 1 - 0x8000];  // chain: next block = blk + 2 + len
    std::printf("block %04X len %u loop %d\n", blk, len, base.loop);
    for (unsigned k = 3; k < 2 + len && blk + k <= 0xFFFF; k++) {
        std::memcpy(patched, gNvs, sizeof patched);
        uint8_t& b = patched[blk + k - 0x8000];
        b = uint8_t(b + delta);
        std::printf("+%02X (%02X->%02X):", k, gNvs[blk + k - 0x8000], b);
        if (!snapBoot(patched, a.id, p)) {
            std::printf(" boot failed\n");
            continue;
        }
        if (p.loop != base.loop) std::printf(" loop %d", p.loop);
        int nw = 0;
        for (int st = 0; st < 128; st++) {
            uint32_t x = 0, y = 0;
            for (int i = 3; i >= 0; i--) {
                x = x << 8 | base.wcs[4 * st + i];
                y = y << 8 | p.wcs[4 * st + i];
            }
            if (x == y) continue;
            if (nw++ >= 6) continue;
            if ((x ^ y) & 0xFFFF) std::printf(" s%d ofs %04X->%04X", st, x & 0xFFFF, y & 0xFFFF);
            if ((x ^ y) >> 26) std::printf(" s%d m %u->%u", st, (~x >> 26) & 63, (~y >> 26) & 63);
            if ((x ^ y) & 0x03FF0000) std::printf(" s%d ctl %03X->%03X", st, (x >> 16) & 0x3FF, (y >> 16) & 0x3FF);
        }
        if (nw > 6) std::printf(" (+%d more steps)", nw - 6);
        int nr = 0;
        std::printf(" | RAM:");
        for (int i = 0; i < 0x800; i++)
            if (base.nvsRam[i] != p.nvsRam[i] && nr++ < 4) std::printf(" %04X", 0x2000 + i);
        for (int i = 0; i < 0x3C0; i++)  // below the stack area
            if (base.sbcRam[i] != p.sbcRam[i] && nr++ < 8) std::printf(" %04X", 0x3C00 + i);
        std::printf(" (%d)\n", nr);
    }
    return 0;
}

// ---- handlers -----------------------------------------------------------
struct HandlerProbe : Probe {
    std::set<uint16_t> handlers;
    int pass = 0, want = 1;
    bool inSlot = false;
    void onFetch(uint16_t a) override {
        if (a == 0xA7A9) {  // decay rebuild entry = start of a builder pass
            ++pass;
            if (pass == want) std::printf("=== builder pass %d\n", pass);
        }
        if (pass != want) return;
        if (a == 0xB000 || a == 0xB00D)
            std::printf("  allpass %04X ret %04X A %02X 3E3E %02X 3E3F %02X 3E12 %02X 3E0A %02X\n", a,
                        peek16(cpu.sp), cpu.a, peek(0x3E3E), peek(0x3E3F), peek(0x3E12), peek(0x3E0A));
        if (a == 0xA853) inSlot = false;
        if (handlers.count(a) && !inSlot) {
            const uint16_t bc = uint16_t(cpu.b << 8 | cpu.c), de = uint16_t(cpu.d << 8 | cpu.e);
            std::printf("slot %04X val %02X Z%d handler %04X BC %04X [%02X %02X %02X]\n", de, peek(de),
                        cpu.fz, a, bc, peek(bc), peek(uint16_t(bc + 1)), peek(uint16_t(bc + 2)));
            inSlot = true;
        }
    }
    void onWrite(uint16_t a, uint8_t v) override {
        if (pass != want || a < 0x4000 || a >= 0x4200) return;
        const int n = (a - 0x4000) >> 2, b = (a - 0x4000) & 3;
        std::printf("    wcs s%d +%d = %02X", n, b, v);
        if (b == 3) std::printf("  m %d", (~v >> 2) & 63);
        if (b == 2) std::printf("  sign %c", (v & 0x80) ? '+' : '-');
        std::printf("  (pc %04X)\n", cpu.pc);
    }
};

int cmdHandlers(const Args& a) {
    if (a.pos.size() > 1) usageExit();
    static HandlerProbe m;
    for (int i = 0; i < 12 * 6; i++) {
        const unsigned t = 0xA91E + 2 * i - 0x8000;
        m.handlers.insert(uint16_t(gNvs[t] | gNvs[t + 1] << 8));
    }
    if (!a.pos.empty()) m.want = std::stoi(a.pos[0]);
    bootOrDie(m, gSbc, gNvs, a.id);
    std::printf("loop %d record %04X\n", m.loopLen, m.recordPtr);
    return 0;
}

// ---- access -------------------------------------------------------------
struct AccessProbe : Probe {
    unsigned lo = 0, hi = 0;
    // (audio, write, addr, pc, value; value only for boot writes) -> count
    std::map<std::tuple<bool, bool, uint16_t, uint16_t, int>, uint64_t> acc;
    void onRead(uint16_t a, uint8_t) override {
        if (a >= lo && a <= hi) acc[{audio, false, a, cpu.pc, -1}]++;
    }
    void onWrite(uint16_t a, uint8_t v) override {
        if (a >= lo && a <= hi) acc[{audio, true, a, cpu.pc, audio ? -1 : v}]++;
    }
};

int cmdAccess(const Args& a) {
    if (a.pos.empty() || a.pos.size() > 2) usageExit();
    static AccessProbe m;
    const auto dash = a.pos[0].find('-');
    m.lo = hexArg(a.pos[0].substr(0, dash));
    m.hi = dash == std::string::npos ? m.lo : hexArg(a.pos[0].substr(dash + 1));
    const double sec = a.pos.size() > 1 ? std::stod(a.pos[1]) : 0.0;
    bootOrDie(m, gSbc, gNvs, a.id);
    if (sec > 0) {
        Silence io;
        m.run(size_t(sec * 30.72e6 / 9.0 / m.loopLen), io);
    }
    std::printf("# phase R/W addr pc [value] count (pc = address after the accessing instruction's operand fetch)\n");
    for (auto& [k, n] : m.acc) {
        auto [aud, wr, addr, pc, v] = k;
        std::printf("%s %c %04X pc %04X", aud ? "audio" : "boot ", wr ? 'W' : 'R', addr, pc);
        if (v >= 0) std::printf(" = %02X", v);
        std::printf("  x%llu\n", (unsigned long long)n);
    }
    return 0;
}

// ---- run ----------------------------------------------------------------
struct RunProbe : Probe {
    long frame = 0;
    std::map<int, uint64_t> wcsw;               // step -> changed-byte writes
    std::map<int, std::set<int>> wcsBytes;      // step -> bytes written
    std::map<uint16_t, uint64_t> xin;           // pc -> IN 06/07 count
    std::map<uint16_t, uint64_t> hits;          // watched opcode address -> fetches
    int traceStep = -1;
    std::vector<std::pair<long, uint32_t>> trace;
    void onFetch(uint16_t a) override {
        if (!audio) return;
        auto it = hits.find(a);
        if (it != hits.end()) it->second++;
    }
    void onWrite(uint16_t a, uint8_t v) override {
        if (!audio || a < 0x4000 || a >= 0x4200) return;
        const int n = (a - 0x4000) >> 2;
        if (wcs()[a - 0x4000] != v) {
            wcsw[n]++;
            wcsBytes[n].insert((a - 0x4000) & 3);
        }
        if (n == traceStep) {
            uint32_t w = wcsWord(n);
            const int sh = 8 * ((a - 0x4000) & 3);
            w = (w & ~(0xFFu << sh)) | uint32_t(v) << sh;
            trace.push_back({frame, w});
        }
    }
    void onIn(uint8_t p) override {
        if (audio && (p == 6 || p == 7)) xin[cpu.pc]++;
    }
};

struct NoiseIo : SampleIo {
    RunProbe* m = nullptr;
    std::mt19937 rng{1};
    std::normal_distribution<double> nd{0.0, 1.0};
    double amp = 0.3 * 32768;
    long burst = 0, b2s = -1, b2e = -1;
    std::vector<std::vector<float>> out{4};
    static int16_t clip(double v) { return int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
    bool nextInput(int16_t& l, int16_t& r) override {
        const long f = m->frame;
        if (f < burst || (f >= b2s && f < b2e)) {
            l = clip(nd(rng) * amp);
            r = clip(nd(rng) * amp);
        } else {
            l = r = 0;
        }
        return true;
    }
    void frame(const DacFrame& d) override {
        ++m->frame;
        for (int c = 0; c < 4; c++) out[c].push_back(d[c] / 32768.0f);
    }
};

int cmdRun(const Args& a) {
    if (a.pos.size() != 3) usageExit();
    static RunProbe m;
    NoiseIo io;
    io.m = &m;
    const double burst = std::stod(a.pos[0]), total = std::stod(a.pos[1]);
    struct Poke {
        uint16_t addr;
        uint8_t v;
        double t;
        bool done;
    };
    std::vector<Poke> pokes;
    std::vector<uint16_t> watch;
    auto opt = [&](const char* k) {
        auto it = a.opts.find(k);
        return it == a.opts.end() ? std::vector<std::string>{} : it->second;
    };
    for (auto& s : opt("amp")) io.amp = std::stod(s) * 32768;
    for (auto& s : opt("trace")) m.traceStep = std::stoi(s);
    for (auto& s : opt("pc")) m.hits[uint16_t(hexArg(s))] = 0;
    for (auto& s : opt("watch")) watch.push_back(uint16_t(hexArg(s)));
    for (auto& s : opt("poke")) {
        const auto eq = s.find('='), at = s.find('@');
        if (eq == std::string::npos || at == std::string::npos || at < eq) usageExit();
        pokes.push_back({uint16_t(hexArg(s.substr(0, eq))), uint8_t(hexArg(s.substr(eq + 1, at - eq - 1))),
                         std::stod(s.substr(at + 1)), false});
    }
    std::string b2;
    for (auto& s : opt("burst2")) b2 = s;

    bootOrDie(m, gSbc, gNvs, a.id);
    const double fs = 30.72e6 / 9.0 / m.loopLen;
    io.burst = long(burst * fs);
    if (!b2.empty()) {
        const auto c = b2.find(',');
        if (c == std::string::npos) usageExit();
        io.b2s = long(std::stod(b2.substr(0, c)) * fs);
        io.b2e = io.b2s + long(std::stod(b2.substr(c + 1)) * fs);
    }
    const long totalN = long(total * fs), chunk = long(fs / 20);
    for (long done = 0; done < totalN; done += chunk) {
        for (auto& p : pokes)
            if (!p.done && done >= p.t * fs) {
                m.poke(p.addr, p.v);
                p.done = true;
            }
        if (!m.run(size_t(chunk), io)) {
            std::printf("DSP stopped\n");
            break;
        }
        if (!watch.empty() && (done / chunk) % 2 == 0) {
            std::printf("w %.2f:", done / fs);
            for (uint16_t w : watch) std::printf(" %04X=%02X", w, m.peek(w));
            std::printf("\n");
        }
    }
    std::string err;
    if (!writeWav(a.pos[2], unsigned(fs + 0.5), io.out, err)) {
        std::printf("%s\n", err.c_str());
        return 1;
    }
    std::printf("fs %.2f loop %d frames %zu\nX reads:", fs, m.loopLen, io.out[0].size());
    for (auto& [pc, c] : m.xin) std::printf(" %04X:%llu", pc, (unsigned long long)c);
    std::printf("\nWCS writes (8080 step:count[bytes]):");
    for (auto& [st, c] : m.wcsw) {
        std::printf(" s%d:%llu[", st, (unsigned long long)c);
        for (int b : m.wcsBytes[st]) std::printf("%d", b);
        std::printf("]");
    }
    std::printf("\npc hits:");
    for (auto& [pc, c] : m.hits) std::printf(" %04X:%llu", pc, (unsigned long long)c);
    std::printf("\n");
    uint32_t last = 0xFFFFFFFF;
    for (auto& [t, w] : m.trace) {
        if (w == last) continue;
        last = w;
        std::printf("  t %.3f s  m %2u ofs %04X\n", t / fs, (~w >> 26) & 63, w & 0xFFFF);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string romDir = LEX224_DEFAULT_ROM_DIR;
    int i = 1;
    if (i + 1 < argc && std::strcmp(argv[i], "--rom-dir") == 0) {
        romDir = argv[i + 1];
        i += 2;
    }
    if (argc - i < 2) usageExit();
    const std::string cmd = argv[i++];
    Args a;
    try {
        a.id = uint8_t(hexArg(argv[i++]));
        for (; i < argc; i++) {
            const std::string s = argv[i];
            if (s.rfind("--", 0) == 0) {
                if (i + 1 >= argc) usageExit();
                a.opts[s.substr(2)].push_back(argv[++i]);
            } else if (s.find('=') != std::string::npos) {
                const auto eq = s.find('=');
                a.patches.push_back({hexArg(s.substr(0, eq)), uint8_t(hexArg(s.substr(eq + 1)))});
            } else {
                a.pos.push_back(s);
            }
        }
    } catch (const std::exception&) {
        usageExit();
    }
    std::string err;
    if (!loadRomFiles(romDir, gSbc, gNvs, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    try {
        applyPatches(a, gSbc, gNvs);
        if (cmd == "steps") return cmdSteps(a);
        if (cmd == "mem") return cmdMem(a);
        if (cmd == "groups") return cmdGroups(a);
        if (cmd == "diff") return cmdDiff(a);
        if (cmd == "handlers") return cmdHandlers(a);
        if (cmd == "access") return cmdAccess(a);
        if (cmd == "run") return cmdRun(a);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bad argument: %s\n", e.what());
        return 2;
    }
    usageExit();
}
