// lex224emu: run the Lexicon 224XL v8.2 firmware and a factory program on a WAV.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "audio.h"
#include "engine.h"
#include "machine.h"
#include "resampler.h"
#include "romfile.h"

namespace {

const char kUsage[] =
    "usage: lex224emu [--rom-dir DIR] [--quad] [--duration SEC] [--tail SEC] PRESET IN.wav "
    "OUT.wav\n"
    "       lex224emu [--rom-dir DIR] --list\n";

int usage() {
    std::fputs(kUsage, stderr);
    return 2;
}

std::string upper(std::string s) {
    for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

bool parseSeconds(const char* s, double& out) {
    char* end = nullptr;
    out = std::strtod(s, &end);
    return end != s && *end == '\0' && std::isfinite(out) && out >= 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string romDir = LEX224_DEFAULT_ROM_DIR;
    bool quad = false, list = false;
    double duration = -1, tail = 3.0;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--rom-dir" && i + 1 < argc) {
            romDir = argv[++i];
        } else if (a == "--quad") {
            quad = true;
        } else if (a == "--list") {
            list = true;
        } else if (a == "--duration" && i + 1 < argc) {
            if (!parseSeconds(argv[++i], duration)) return usage();
        } else if (a == "--tail" && i + 1 < argc) {
            if (!parseSeconds(argv[++i], tail)) return usage();
        } else if (a.size() > 1 && a[0] == '-') {
            return usage();
        } else {
            pos.push_back(a);
        }
    }
    if (list ? !pos.empty() : pos.size() != 3) return usage();

    std::string err;
    // static: large fixed-size buffers stay off the stack.
    static uint8_t sbc[0x1800], nvs[0x8000];
    if (!loadRomFiles(romDir, sbc, nvs, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    const RomImage roms{sbc, nvs};
    ProgramList progs;
    listPrograms(roms, progs);
    if (list) {
        for (int i = 0; i < progs.count; i++) std::printf("%s\n", progs.items[i].name);
        return 0;
    }

    const std::string want = upper(trim(pos[0]));
    const Program* prog = nullptr;
    for (int i = 0; i < progs.count && !prog; i++)
        if (upper(progs.items[i].name) == want) prog = &progs.items[i];
    if (!prog) {
        std::fprintf(stderr, "unknown preset \"%s\"; available:\n", pos[0].c_str());
        for (int i = 0; i < progs.count; i++) std::fprintf(stderr, "%s\n", progs.items[i].name);
        return 2;
    }

    unsigned inRate = 0, channels = 0;
    std::vector<std::vector<float>> in;
    if (!readWav(pos[1], inRate, channels, in, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::vector<float> inL = in[0];
    std::vector<float> inR = channels >= 2 ? in[1] : in[0];
    in.clear();
    if (duration >= 0) {
        const size_t n = std::min(inL.size(), size_t(std::llround(duration * inRate)));
        inL.resize(n);
        inR.resize(n);
    }
    const size_t inFrames = inL.size();

    // static: large fixed-size buffers stay off the stack.
    static float kernel[kKernelTableSize];
    static int16_t dmem[65536];
    static Engine<2, 4> engine;
    initKernelTable(kernel);
    EngineConfig cfg{};
    cfg.roms = roms;
    cfg.dmem = dmem;
    cfg.kernel = kernel;
    cfg.sampleRate = inRate;
    cfg.programId = prog->id;
    cfg.numInputs = channels >= 2 ? 2 : 1;
    // One DAC per output: stereo L = DAC A, R = DAC C; --quad A, B, C, D.
    const std::vector<uint8_t> mixes = quad ? std::vector<uint8_t>{1, 2, 4, 8}
                                            : std::vector<uint8_t>{1, 4};
    cfg.numOutputs = int(mixes.size());
    std::copy(mixes.begin(), mixes.end(), cfg.outMix);
    const char* engineErr = nullptr;
    if (!engine.start(cfg, &engineErr)) {
        std::fprintf(stderr, "%s\n", engineErr);
        return 1;
    }

    // The engine delays by latency() samples: run that much past the end and
    // drop the head.
    const size_t nOutFinal = inFrames + size_t(std::llround(tail * inRate));
    const size_t latency = size_t(engine.latency());
    const size_t total = nOutFinal + latency;
    std::vector<std::vector<float>> out(mixes.size(), std::vector<float>(nOutFinal));
    constexpr size_t kBlock = 48;
    float inBuf[2][kBlock], outBuf[4][kBlock];
    const float* inPtr[2] = {inBuf[0], inBuf[1]};
    float* outPtr[4] = {outBuf[0], outBuf[1], outBuf[2], outBuf[3]};
    for (size_t done = 0; done < total;) {
        const size_t n = std::min(kBlock, total - done);
        for (size_t i = 0; i < n; i++) {
            const size_t k = done + i;
            inBuf[0][i] = k < inFrames ? inL[k] : 0.0f;
            inBuf[1][i] = k < inFrames ? inR[k] : 0.0f;
        }
        if (!engine.process(inPtr, outPtr, n)) {
            std::fprintf(stderr, "DSP halted during processing\n");
            return 1;
        }
        for (size_t i = 0; i < n; i++) {
            const size_t k = done + i;
            if (k < latency) continue;
            for (size_t c = 0; c < mixes.size(); c++) out[c][k - latency] = outBuf[c][i];
        }
        done += n;
    }
    std::fprintf(stderr,
                 "program %s (id %02X) record %04X, loop %d steps, DSP rate %.2f Hz, boot %.2f s, "
                 "WCS writes during audio %lld\n",
                 prog->name, prog->id, engine.recordPtr(), engine.loopLen(), engine.dspRate(),
                 engine.bootSeconds(), static_cast<long long>(engine.wcsWritesAudio()));

    if (!writeWav(pos[2], inRate, out, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    return 0;
}
