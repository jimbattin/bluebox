// BARC editor link over USB MIDI, host-portable (no libDaisy): SysEx framing,
// the command parser, the USB-MIDI packetizer and a lock-free SPSC queue.
// The browser editor (editor/src/protocol.ts at the repo root) speaks the same bytes;
// both sides are tested against the same vectors (tests/editor_protocol_test.cpp,
// editor/src/protocol.test.ts).
//
// Frame: F0 7D 42 01 <cmd> <payload> F7 (7D = non-commercial SysEx ID).
// An 8-bit value v travels as (v>>7)&1, v&0x7F; a 14-bit value as (v>>7)&0x7F, v&0x7F;
// a 32-bit value as 5 bytes: (v>>28)&0x0F, (v>>21)&0x7F, (v>>14)&0x7F, (v>>7)&0x7F, v&0x7F.
// ROM slots: 0-2 SBC1-3 (2716, 2 KiB), 3-10 NVS1-8 (2732, 4 KiB).
// Host -> device:
//   01 HELLO      (none): the device answers with STATE, every PAGE and ROMS
//   02 PROGRAM    index 0..count-1
//   03 SLIDER     page 1-12, slider 0-5, value(8)
//   04 OPTION     bit 0 DYN DECAY / 6 MODE ENH / 7 DECAY OPT, on 0/1
//   05 MIX        value(14), 0 dry .. kMixMax wet
//   06 BYPASS     0/1
//   07 ROM_BEGIN  slot: starts an upload (answered by ROM_ACK next 0)
//   08 ROM_DATA   slot, offset(14) (a multiple of kRomChunk), kRomChunk x value(8); answered
//                 by ROM_ACK. Chunks go in order; a resent earlier chunk is acked again, a
//                 gap ends the upload with ROM_RESULT kRomErrSequence.
//   09 ROM_COMMIT slot, crc(32) of the whole image: checked, written to flash, ROM_RESULT
//   0A ROM_ERASE  slot: erases its flash copy (a built-in image takes over), ROM_RESULT
//   0B OUTMIX     mix: the loaded program's mono output, an EngineConfig::outMix mask: 00 the
//                 program's default, 01 A, 04 C, 05 A+C, 0F A+B+C+D (IsOutMixChoice); kept per
//                 program in flash
// Device -> host:
//   10 STATE      index, count, id(8), runState (0 loading, 1 running, 2 failed, 3 no ROMs),
//                 bypass, options(8), mix(14), pageMask(14) (bit p-1 = page p present),
//                 outMix (the override, 0 = default), outDefault (the program's default mask)
//   11 PAGE       page, 6 x value(8): the firmware's slider registers (clamped)
//   12 LOG        printable ASCII, at most kMaxLog chars
//   13 ROMS       embedded (1: the build carries built-in images), kRomSlots x [state
//                 (RomState), crc(32) of the image in use (0 if empty)]
//   14 ROM_ACK    slot, next(14): the next offset the device wants (RomSize once complete)
//   15 ROM_RESULT slot, code (RomResult)
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace barc {

constexpr uint8_t kManufacturer = 0x7D, kDevice = 0x42, kVersion = 0x01;
constexpr uint16_t kMixMax = 16383;
constexpr int kRomSlots = 11;
constexpr size_t kRomChunk = 64, kRomMaxSize = 4096;
constexpr size_t kMaxFrame = 4 + 3 + 2 * kRomChunk;  // data bytes between F0 and F7: a ROM_DATA frame
constexpr size_t kMaxLog = 120;

constexpr uint16_t RomSize(int slot) { return slot < 3 ? 2048 : 4096; }
// The byte sum each ROM must have: its socket index (the ROM's own checksum test).
constexpr uint8_t RomSocket(int slot) { return uint8_t(slot < 3 ? slot + 1 : slot - 2); }
// The editor's mono-output menu: 0 (the program's default) or an EngineConfig::outMix mask.
constexpr bool IsOutMixChoice(uint8_t m) {
    return m == 0x00 || m == 0x01 || m == 0x04 || m == 0x05 || m == 0x0F;
}

inline uint8_t RomByteSum(const uint8_t* p, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s = uint8_t(s + p[i]);
    return s;
}

// IEEE CRC-32 (reflected, polynomial 0xEDB88320, init and final xor 0xFFFFFFFF).
inline uint32_t Crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

enum Cmd : uint8_t {
    kHello = 0x01,
    kProgram = 0x02,
    kSlider = 0x03,
    kOption = 0x04,
    kMix = 0x05,
    kBypass = 0x06,
    kRomBegin = 0x07,
    kRomData = 0x08,
    kRomCommit = 0x09,
    kRomErase = 0x0A,
    kOutMix = 0x0B,
    kState = 0x10,
    kPage = 0x11,
    kLog = 0x12,
    kRoms = 0x13,
    kRomAck = 0x14,
    kRomResult = 0x15,
};

enum RomState : uint8_t { kRomEmpty = 0, kRomFlash = 1, kRomBuiltIn = 2 };

enum RomResult : uint8_t {
    kRomOk = 0,
    kRomErrSequence = 1,
    kRomErrIncomplete = 2,
    kRomErrCrc = 3,
    kRomErrSocket = 4,
    kRomErrFlash = 5,
    kRomErrVerify = 6,
};

// A decoded host command. PROGRAM: a = index. SLIDER: a = page, b = slider, value.
// OPTION: a = mask (1 << bit), b = on. MIX: value. BYPASS: b. HELLO: no fields.
// ROM_*: a = slot; ROM_DATA: value = offset (bytes in Parser::Chunk()); ROM_COMMIT: crc.
// OUTMIX: a = mix.
struct Command {
    uint8_t cmd, a, b;
    uint16_t value;
    uint32_t crc;
};

// Byte-at-a-time SysEx parser. Realtime bytes (F8-FF) are ignored anywhere; F0
// starts a frame; any other status byte except F7 aborts it; F7 decodes it.
class Parser {
public:
    // True when `byte` completes a valid command, written to `out`.
    bool Feed(uint8_t byte, Command& out) {
        if (byte >= 0xF8) return false;
        if (byte == 0xF0) {
            inFrame_ = true;
            overflow_ = false;
            len_ = 0;
            return false;
        }
        if (byte == 0xF7) {
            const bool complete = inFrame_ && !overflow_;
            inFrame_ = false;
            return complete && Decode(out);
        }
        if (byte & 0x80) {
            inFrame_ = false;
            return false;
        }
        if (!inFrame_) return false;
        if (len_ < kMaxFrame)
            buf_[len_++] = byte;
        else
            overflow_ = true;
        return false;
    }
    // The last ROM_DATA's kRomChunk bytes, valid until the next Feed() that completes a command.
    const uint8_t* Chunk() const { return chunk_; }

private:
    bool Decode(Command& out) {
        if (len_ < 4 || buf_[0] != kManufacturer || buf_[1] != kDevice || buf_[2] != kVersion)
            return false;
        const uint8_t* p = buf_ + 4;
        const size_t n = len_ - 4;
        Command c{buf_[3], 0, 0, 0, 0};
        switch (c.cmd) {
            case kHello:
                if (n != 0) return false;
                break;
            case kProgram:
                if (n != 1 || p[0] >= 32) return false;
                c.a = p[0];
                break;
            case kSlider:
                if (n != 4 || p[0] < 1 || p[0] > 12 || p[1] > 5 || p[2] > 1) return false;
                c.a = p[0];
                c.b = p[1];
                c.value = uint16_t(p[2] << 7 | p[3]);
                break;
            case kOption:
                if (n != 2 || !(p[0] == 0 || p[0] == 6 || p[0] == 7) || p[1] > 1) return false;
                c.a = uint8_t(1u << p[0]);
                c.b = p[1];
                break;
            case kMix:
                if (n != 2) return false;
                c.value = uint16_t(p[0] << 7 | p[1]);  // <= kMixMax by construction
                break;
            case kBypass:
                if (n != 1 || p[0] > 1) return false;
                c.b = p[0];
                break;
            case kRomBegin:
            case kRomErase:
                if (n != 1 || p[0] >= kRomSlots) return false;
                c.a = p[0];
                break;
            case kRomData: {
                if (n != 3 + 2 * kRomChunk || p[0] >= kRomSlots) return false;
                const uint16_t offset = uint16_t(p[1] << 7 | p[2]);
                if (offset % kRomChunk != 0 || offset + kRomChunk > RomSize(p[0])) return false;
                for (size_t i = 0; i < kRomChunk; i++)
                    if (p[3 + 2 * i] > 1) return false;
                for (size_t i = 0; i < kRomChunk; i++)
                    chunk_[i] = uint8_t(p[3 + 2 * i] << 7 | p[4 + 2 * i]);
                c.a = p[0];
                c.value = offset;
                break;
            }
            case kRomCommit:
                if (n != 6 || p[0] >= kRomSlots || p[1] > 0x0F) return false;
                c.a = p[0];
                c.crc = uint32_t(p[1]) << 28 | uint32_t(p[2]) << 21 | uint32_t(p[3]) << 14 |
                        uint32_t(p[4]) << 7 | p[5];
                break;
            case kOutMix:
                if (n != 1 || !IsOutMixChoice(p[0])) return false;
                c.a = p[0];
                break;
            default:
                return false;
        }
        out = c;
        return true;
    }

    uint8_t buf_[kMaxFrame] = {};
    uint8_t chunk_[kRomChunk] = {};
    size_t len_ = 0;
    bool inFrame_ = false, overflow_ = false;
};

struct StateMsg {
    uint8_t programIndex, programCount, programId, runState;  // runState 0 loading, 1 running, 2 failed, 3 no ROMs
    bool bypass;
    uint8_t options;
    uint16_t mix, pageMask;  // pageMask bit p-1 = page p present
    uint8_t outMix, outDefault;  // override (0 = default) and the program's default, outMix masks

    bool operator==(const StateMsg& o) const {
        return programIndex == o.programIndex && programCount == o.programCount &&
               programId == o.programId && runState == o.runState && bypass == o.bypass &&
               options == o.options && mix == o.mix && pageMask == o.pageMask &&
               outMix == o.outMix && outDefault == o.outDefault;
    }
    bool operator!=(const StateMsg& o) const { return !(*this == o); }
};

// Device messages packed as USB-MIDI event packets (cable 0), ready for one bulk
// transfer: SysEx start/continue packets (CIN 4) of 3 bytes, then an end packet
// of 1/2/3 bytes (CIN 5/6/7), zero-padded.
class TxBuffer {
public:
    static constexpr size_t kCapacity = 1024;

    bool AppendState(const StateMsg& s) {
        const uint8_t p[14] = {uint8_t(s.programIndex & 0x7F), uint8_t(s.programCount & 0x7F),
                               uint8_t(s.programId >> 7 & 1),  uint8_t(s.programId & 0x7F),
                               uint8_t(s.runState & 0x7F),     uint8_t(s.bypass ? 1 : 0),
                               uint8_t(s.options >> 7 & 1),    uint8_t(s.options & 0x7F),
                               uint8_t(s.mix >> 7 & 0x7F),     uint8_t(s.mix & 0x7F),
                               uint8_t(s.pageMask >> 7 & 0x7F), uint8_t(s.pageMask & 0x7F),
                               uint8_t(s.outMix & 0x7F),       uint8_t(s.outDefault & 0x7F)};
        return AppendSysEx(kState, p, sizeof p);
    }
    bool AppendPage(int page, const uint8_t v[6]) {
        uint8_t p[13];
        p[0] = uint8_t(page & 0x7F);
        for (int i = 0; i < 6; i++) {
            p[1 + 2 * i] = uint8_t(v[i] >> 7 & 1);
            p[2 + 2 * i] = uint8_t(v[i] & 0x7F);
        }
        return AppendSysEx(kPage, p, sizeof p);
    }
    // Printable ASCII; other bytes become '?'. Truncated to kMaxLog chars.
    bool AppendLog(const char* text) {
        uint8_t p[kMaxLog];
        size_t n = 0;
        for (; n < kMaxLog && text[n]; n++) {
            const uint8_t c = uint8_t(text[n]);
            p[n] = c >= 0x20 && c <= 0x7E ? c : uint8_t('?');
        }
        return AppendSysEx(kLog, p, n);
    }
    // `state` and `crc` per slot (crc 0 for empty slots).
    bool AppendRoms(bool embedded, const uint8_t state[kRomSlots], const uint32_t crc[kRomSlots]) {
        uint8_t p[1 + 6 * kRomSlots];
        p[0] = embedded ? 1 : 0;
        for (int i = 0; i < kRomSlots; i++) {
            p[1 + 6 * i] = uint8_t(state[i] & 0x7F);
            Put32(p + 2 + 6 * i, crc[i]);
        }
        return AppendSysEx(kRoms, p, sizeof p);
    }
    bool AppendRomAck(uint8_t slot, uint16_t next) {
        const uint8_t p[3] = {uint8_t(slot & 0x7F), uint8_t(next >> 7 & 0x7F), uint8_t(next & 0x7F)};
        return AppendSysEx(kRomAck, p, sizeof p);
    }
    bool AppendRomResult(uint8_t slot, uint8_t code) {
        const uint8_t p[2] = {uint8_t(slot & 0x7F), uint8_t(code & 0x7F)};
        return AppendSysEx(kRomResult, p, sizeof p);
    }

    const uint8_t* data() const { return buf_; }
    size_t size() const { return size_; }
    void Clear() { size_ = 0; }

private:
    static void Put32(uint8_t* p, uint32_t v) {
        p[0] = uint8_t(v >> 28 & 0x0F);
        p[1] = uint8_t(v >> 21 & 0x7F);
        p[2] = uint8_t(v >> 14 & 0x7F);
        p[3] = uint8_t(v >> 7 & 0x7F);
        p[4] = uint8_t(v & 0x7F);
    }
    // All or nothing: false (and nothing appended) if the message does not fit.
    bool AppendSysEx(uint8_t cmd, const uint8_t* payload, size_t n) {
        uint8_t msg[6 + kMaxLog];
        if (n > kMaxLog) return false;
        size_t len = 0;
        msg[len++] = 0xF0;
        msg[len++] = kManufacturer;
        msg[len++] = kDevice;
        msg[len++] = kVersion;
        msg[len++] = cmd;
        for (size_t i = 0; i < n; i++) msg[len++] = payload[i];
        msg[len++] = 0xF7;
        if (size_ + (len + 2) / 3 * 4 > kCapacity) return false;
        size_t i = 0;
        for (; len - i > 3; i += 3) Packet(0x04, msg + i, 3);
        Packet(uint8_t(0x04 + (len - i)), msg + i, len - i);  // 1/2/3 -> CIN 5/6/7
        return true;
    }
    void Packet(uint8_t cin, const uint8_t* b, size_t n) {
        uint8_t* q = buf_ + size_;
        q[0] = cin;
        for (size_t i = 0; i < 3; i++) q[1 + i] = i < n ? b[i] : 0;
        size_ += 4;
    }

    uint8_t buf_[kCapacity];
    size_t size_ = 0;
};

// Staging buffer for one ROM upload (ROM_BEGIN / ROM_DATA / ROM_COMMIT), host-portable.
class RomUpload {
public:
    void Begin(uint8_t slot) {
        slot_ = slot;
        received_ = 0;
        active_ = true;
    }
    // Takes the chunk at `offset`: the next one is stored, an earlier one (a resend)
    // is ignored. False, and the upload is cancelled, for another slot, a gap or no
    // upload. On true, `next` is the next offset wanted.
    bool Data(uint8_t slot, uint16_t offset, const uint8_t* chunk, uint16_t& next) {
        if (!active_ || slot != slot_ || offset > received_ || offset + kRomChunk > RomSize(slot)) {
            active_ = false;
            return false;
        }
        if (offset == received_) {
            std::memcpy(buf_ + offset, chunk, kRomChunk);
            received_ = uint16_t(received_ + kRomChunk);
        }
        next = received_;
        return true;
    }
    // Ends the upload in every case; kRomOk if the image is complete, has CRC-32 `crc`
    // and the socket's byte sum.
    uint8_t Commit(uint8_t slot, uint32_t crc) {
        const bool active = active_;
        active_ = false;
        if (!active || slot != slot_) return kRomErrSequence;
        const uint16_t size = RomSize(slot);
        if (received_ != size) return kRomErrIncomplete;
        if (Crc32(buf_, size) != crc) return kRomErrCrc;
        if (RomByteSum(buf_, size) != RomSocket(slot)) return kRomErrSocket;
        return kRomOk;
    }
    const uint8_t* Image() const { return buf_; }

private:
    uint8_t buf_[kRomMaxSize] = {};
    uint16_t received_ = 0;
    uint8_t slot_ = 0;
    bool active_ = false;
};

// Single-producer single-consumer ring of capacity N (a power of two), safe
// between an interrupt and the main loop: free-running indices, the producer
// publishes with release, the consumer reads with acquire.
template <class T, size_t N>
class SpscQueue {
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be a power of two");

public:
    bool Push(const T& v) {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == N) return false;
        buf_[h & (N - 1)] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool Pop(T& v) {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (head_.load(std::memory_order_acquire) == t) return false;
        v = buf_[t & (N - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

private:
    T buf_[N];
    std::atomic<uint32_t> head_{0}, tail_{0};
};

}  // namespace barc
