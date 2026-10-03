// Real-time 224XL engine: host-rate audio in, host-rate audio out, fixed latency.
// Input is pre-emphasised (224X AIN shelf, emphasis.h), resampled to the DSP rate
// and run through the emulated machine sample by sample; the chosen DAC channels
// (EngineConfig::outMix, changeable live with setOutMix) are resampled back and
// de-emphasised (AOUT). Allocation-free: all buffers are members; ROMs, DMEM and the
// kernel table are caller-owned.
#pragma once
#include <cstddef>
#include <cstdint>

#include "emphasis.h"
#include "machine.h"
#include "resampler.h"

struct EngineConfig {
    RomImage roms;
    int16_t* dmem;          // 65536 words
    const float* kernel;    // kKernelTableSize floats from initKernelTable
    uint32_t sampleRate;    // 8000..192000
    uint8_t programId;
    int numInputs;          // 1..MaxIn; with 1, DSP L and R both get the one input
    int numOutputs;         // 1..MaxOut
    uint8_t outMix[4];      // per output: DACs to average, bit0 A .. bit3 D (1..15)
    uint8_t optionMask;     // option bits (3CCD) forced at program load, see Machine::setBootOptions
    uint8_t optionBits;
};

template <int MaxIn, int MaxOut> class Engine {
public:
    static constexpr size_t kMaxBlock = 64;

    Engine() : io_(*this) {}

    // Full reset and boot; safe to call repeatedly. `err` is set on failure.
    bool start(const EngineConfig& cfg, const char** err);
    // in[c] for c < numInputs, out[k] for k < numOutputs; n <= kMaxBlock.
    // False: not started, n too large, or the DSP stopped.
    bool process(const float* const* in, float* const* out, size_t n);

    int latency() const { return latency_; }  // samples at sampleRate
    int loopLen() const { return machine_.loopLen; }
    double dspRate() const {
        return double(Machine::kMasterHz) / Machine::kTicksPerDspStep / machine_.loopLen;
    }
    uint16_t recordPtr() const { return machine_.recordPtr; }
    double bootSeconds() const { return machine_.bootSeconds; }
    int64_t wcsWritesAudio() const { return machine_.wcsWritesAudio; }
    // Output-FIFO underruns plus machine input underruns since start().
    uint32_t underruns() const { return fifoUnderruns_ + machine_.inputUnderruns; }

    // Live LARC controls, applied by the emulated firmware (Machine::moveSlider/setOption).
    // False before a successful start().
    bool moveSlider(int page, int slider, uint8_t position) {
        return started_ && machine_.moveSlider(page, slider, position);
    }
    bool setOption(uint8_t mask, bool on) { return started_ && machine_.setOption(mask, on); }
    bool hasPage(int page) const { return started_ && machine_.hasPage(page); }
    // Firmware slider registers out[page-1][slider] (absent pages 0) and option bits;
    // false / 0 before a successful start().
    bool sliderValues(uint8_t (&out)[12][6]) const {
        if (!started_) return false;
        machine_.sliderValues(out);
        return true;
    }
    uint8_t options() const { return started_ ? machine_.options() : 0; }
    // Output k's DAC mix (1..15, as EngineConfig::outMix), from the next DSP frame on.
    // False before a successful start(), or for a bad k or mix.
    bool setOutMix(int k, uint8_t mix);

private:
    // Sizes are powers of two: ring indices are masked.
    static constexpr size_t kRing = 1024;   // resampler input rings
    static constexpr size_t kQueue = 512;   // DSP-rate input queue
    static constexpr size_t kFifo = 1024;   // host-rate output FIFO
    // Inputs kept queued so that Machine::run's WCS-stall overshoot still finds input.
    static constexpr uint32_t kWithheld = 3;

    struct Io final : SampleIo {
        explicit Io(Engine& e) : e(e) {}
        bool nextInput(int16_t& l, int16_t& r) override;
        void frame(const DacFrame& dac) override;
        Engine& e;
    };

    Machine machine_;
    StreamResampler<MaxIn, kRing> in_;
    StreamResampler<MaxOut, kRing> out_;
    Shelf1 pre_[MaxIn];   // AIN pre-emphasis, host rate
    Shelf1 de_[MaxOut];   // AOUT de-emphasis, host rate
    int16_t qL_[kQueue], qR_[kQueue];
    uint32_t qHead_ = 0, qTail_ = 0;
    float fifo_[MaxOut][kFifo];
    uint32_t fRead_ = 0, fWrite_ = 0;
    Io io_;
    EngineConfig cfg_{};
    float outScale_[MaxOut] = {};  // 1 / (32768 * DACs in outMix)
    int latency_ = 0;
    uint32_t fifoUnderruns_ = 0;
    bool started_ = false;
};
