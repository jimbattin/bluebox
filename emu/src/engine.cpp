// Engine: config checks and boot, then per block: pre-emphasis, input resampler,
// machine, output resampler, de-emphasis, output FIFO.
#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// Host float (full scale 1.0) to the machine's 16-bit input word, saturating.
int16_t toInt16(float x) {
    return int16_t(std::clamp(std::lround(double(x) * 32768.0), -32768L, 32767L));
}

}  // namespace

template <int MaxIn, int MaxOut>
bool Engine<MaxIn, MaxOut>::Io::nextInput(int16_t& l, int16_t& r) {
    if (e.qHead_ == e.qTail_) return false;
    const size_t i = e.qTail_ & (kQueue - 1);
    l = e.qL_[i];
    r = e.qR_[i];
    e.qTail_++;
    return true;
}

template <int MaxIn, int MaxOut> void Engine<MaxIn, MaxOut>::Io::frame(const DacFrame& dac) {
    float f[MaxOut];
    for (int k = 0; k < MaxOut; k++) {
        if (k >= e.cfg_.numOutputs) {
            f[k] = 0.0f;
            continue;
        }
        const unsigned mix = e.cfg_.outMix[k];
        int32_t sum = 0;
        for (int d = 0; d < 4; d++)
            if (mix >> d & 1) sum += dac[d];
        f[k] = float(sum) * e.outScale_[k];
    }
    e.out_.push(f);
}

template <int MaxIn, int MaxOut>
bool Engine<MaxIn, MaxOut>::start(const EngineConfig& cfg, const char** err) {
    started_ = false;
    bool ok = cfg.dmem && cfg.kernel && cfg.roms.sbc && cfg.roms.nvs && cfg.numInputs >= 1 &&
              cfg.numInputs <= MaxIn && cfg.numOutputs >= 1 && cfg.numOutputs <= MaxOut;
    for (int k = 0; ok && k < cfg.numOutputs; k++) ok = cfg.outMix[k] >= 1 && cfg.outMix[k] <= 15;
    if (!ok) {
        *err = "bad engine config";
        return false;
    }
    if (cfg.sampleRate < 8000 || cfg.sampleRate > 192000) {
        *err = "sample rate must be 8000-192000 Hz";
        return false;
    }
    if (!romChecksumsOk(cfg.roms)) {
        *err = "ROM checksum mismatch";
        return false;
    }
    cfg_ = cfg;
    for (int k = 0; k < cfg.numOutputs; k++)  // mean of the mixed DACs, full scale 1.0
        outScale_[k] = 1.0f / (32768.0f * float(__builtin_popcount(cfg.outMix[k])));
    machine_.reset(cfg.roms, cfg.dmem);
    machine_.setBootOptions(cfg.optionMask, cfg.optionBits);
    if (!machine_.boot(cfg.programId, err)) return false;

    const uint32_t fs = cfg.sampleRate;
    for (Shelf1& s : pre_) s.init(fs, kEmphasisTauZero, kEmphasisTauPole);
    for (Shelf1& s : de_) s.init(fs, kEmphasisTauPole, kEmphasisTauZero);
    const uint32_t dspTicks = uint32_t(Machine::kTicksPerDspStep) * uint32_t(machine_.loopLen);
    const uint32_t master = uint32_t(Machine::kMasterHz);
    in_.init(cfg.kernel, fs * dspTicks, master);
    out_.init(cfg.kernel, master, fs * dspTicks);
    // Output FIFO pre-roll in host samples: the input resampler's look-ahead (its
    // half-width, host samples) plus the output resampler's (DSP samples, x fs/D),
    // with 4 DSP samples and 2 host samples of margin. engine_test requires no
    // underrun with it at 8, 44.1, 48, 96 and 192 kHz with blocks of 1 and 64.
    const double D = dspRate();
    latency_ = int(std::ceil(in_.halfWidth() + (out_.halfWidth() + 4.0) * fs / D)) + 2;

    qHead_ = qTail_ = 0;
    std::memset(fifo_, 0, sizeof fifo_);
    fRead_ = 0;
    fWrite_ = uint32_t(latency_);
    fifoUnderruns_ = 0;
    started_ = true;
    return true;
}

template <int MaxIn, int MaxOut>
bool Engine<MaxIn, MaxOut>::setOutMix(int k, uint8_t mix) {
    if (!started_ || k < 0 || k >= cfg_.numOutputs || mix < 1 || mix > 15) return false;
    cfg_.outMix[k] = mix;
    outScale_[k] = 1.0f / (32768.0f * float(__builtin_popcount(mix)));
    return true;
}

template <int MaxIn, int MaxOut>
bool Engine<MaxIn, MaxOut>::process(const float* const* in, float* const* out, size_t n) {
    if (!started_ || n > kMaxBlock) return false;

    for (size_t i = 0; i < n; i++) {
        float f[MaxIn];
        for (int c = 0; c < MaxIn; c++) f[c] = pre_[c].process(in[c < cfg_.numInputs ? c : 0][i]);
        in_.push(f);
    }
    while (in_.ready()) {
        float f[MaxIn];
        in_.pull(f);
        const size_t i = qHead_ & (kQueue - 1);
        qL_[i] = toInt16(f[0]);
        // numInputs >= 2 implies MaxIn >= 2; the inner ?: only keeps the index in bounds
        // for MaxIn == 1.
        qR_[i] = toInt16(cfg_.numInputs >= 2 ? f[MaxIn > 1 ? 1 : 0] : f[0]);
        qHead_++;
    }

    const uint32_t queued = qHead_ - qTail_;
    if (queued > kWithheld && !machine_.run(queued - kWithheld, io_)) return false;

    while (out_.ready()) {
        float f[MaxOut];
        out_.pull(f);
        const size_t w = fWrite_ & (kFifo - 1);
        for (int k = 0; k < MaxOut; k++) fifo_[k][w] = de_[k].process(f[k]);
        fWrite_++;
    }

    for (size_t i = 0; i < n; i++) {
        if (fRead_ != fWrite_) {
            const size_t r = fRead_ & (kFifo - 1);
            for (int k = 0; k < cfg_.numOutputs; k++) out[k][i] = fifo_[k][r];
            fRead_++;
        } else {
            for (int k = 0; k < cfg_.numOutputs; k++) out[k][i] = 0.0f;
            fifoUnderruns_++;
        }
    }
    return true;
}

// Engine<1, 1>: pedal and bench; Engine<2, 4>: lex224emu. engine_test runs both.
template class Engine<1, 1>;
template class Engine<2, 4>;
