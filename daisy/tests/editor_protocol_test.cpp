// Host tests for the BARC editor protocol (src/editor_protocol.h). The byte vectors
// are the ones editor/src/protocol.test.ts (repo root) checks the browser side against.
#include <stdint.h>
#include <string.h>

#include <initializer_list>
#include <vector>

#include "check.h"
#include "editor_protocol.h"

using namespace barc;

// Feeds `bytes`; returns the number of commands decoded, the last one in `out`.
static int Feed(Parser& p, const std::vector<uint8_t>& bytes, Command& out) {
    int n = 0;
    for (uint8_t b : bytes) n += p.Feed(b, out) ? 1 : 0;
    return n;
}

static bool One(std::initializer_list<uint8_t> bytes, Command& out) {
    Parser p;
    return Feed(p, bytes, out) == 1;
}

// A ROM_DATA frame with chunk bytes f(i); `highTwo` puts 2 in the first value's high byte.
template <class F>
static std::vector<uint8_t> RomData(uint8_t slot, uint16_t offset, F f, bool highTwo = false) {
    std::vector<uint8_t> v = {0xF0, 0x7D, 0x42, 0x01, 0x08, slot, uint8_t(offset >> 7 & 0x7F),
                              uint8_t(offset & 0x7F)};
    for (size_t i = 0; i < kRomChunk; i++) {
        const uint8_t b = f(i);
        v.push_back(uint8_t(b >> 7));
        v.push_back(uint8_t(b & 0x7F));
    }
    if (highTwo) v[8] = 2;
    v.push_back(0xF7);
    return v;
}

// Feeds the first `chunks` chunks of `img` to `u`; true if every chunk was taken and acked.
static bool Upload(RomUpload& u, uint8_t slot, const uint8_t* img, size_t chunks) {
    bool ok = true;
    for (size_t k = 0; k < chunks; k++) {
        uint16_t next = 0;
        ok = u.Data(slot, uint16_t(k * kRomChunk), img + k * kRomChunk, next) &&
             next == (k + 1) * kRomChunk && ok;
    }
    return ok;
}

// The SysEx bytes carried by a buffer of USB-MIDI packets (CIN 4 = 3 bytes, 5/6/7 = 1/2/3).
static std::vector<uint8_t> Unpack(const TxBuffer& t) {
    std::vector<uint8_t> v;
    for (size_t i = 0; i + 4 <= t.size(); i += 4) {
        const uint8_t cin = t.data()[i] & 0x0F;
        const int n = cin == 0x04 ? 3 : cin >= 0x05 && cin <= 0x07 ? cin - 0x04 : 0;
        for (int k = 0; k < n; k++) v.push_back(t.data()[i + 1 + k]);
    }
    return v;
}

static bool Same(const TxBuffer& t, std::initializer_list<uint8_t> want) {
    return t.size() == want.size() && memcmp(t.data(), want.begin(), want.size()) == 0;
}

static bool SameSysEx(const TxBuffer& t, std::initializer_list<uint8_t> want) {
    return Unpack(t) == std::vector<uint8_t>(want);
}

int main() {
    Command c{};

    // Host -> device.
    CHECK(One({0xF0, 0x7D, 0x42, 0x01, 0x01, 0xF7}, c) && c.cmd == kHello);
    CHECK(One({0xF0, 0x7D, 0x42, 0x01, 0x02, 0x0D, 0xF7}, c) && c.cmd == kProgram && c.a == 13);
    CHECK(One({0xF0, 0x7D, 0x42, 0x01, 0x03, 0x01, 0x01, 0x01, 0x40, 0xF7}, c) && c.cmd == kSlider &&
          c.a == 1 && c.b == 1 && c.value == 0xC0);
    CHECK(One({0xF0, 0x7D, 0x42, 0x01, 0x04, 0x06, 0x01, 0xF7}, c) && c.cmd == kOption &&
          c.a == 0x40 && c.b == 1);
    CHECK(One({0xF0, 0x7D, 0x42, 0x01, 0x04, 0x00, 0x00, 0xF7}, c) && c.cmd == kOption &&
          c.a == 0x01 && c.b == 0);
    CHECK(One({0xF0, 0x7D, 0x42, 0x01, 0x05, 0x7F, 0x7F, 0xF7}, c) && c.cmd == kMix &&
          c.value == kMixMax);
    CHECK(One({0xF0, 0x7D, 0x42, 0x01, 0x06, 0x01, 0xF7}, c) && c.cmd == kBypass && c.b == 1);

    // Rejected frames.
    CHECK(!One({0xF0, 0x7E, 0x42, 0x01, 0x01, 0xF7}, c));                    // manufacturer
    CHECK(!One({0xF0, 0x7D, 0x42, 0x02, 0x01, 0xF7}, c));                    // version 02
    CHECK(!One({0xF0, 0x7D, 0x42, 0x01, 0x03, 0x0D, 0x01, 0x00, 0x10, 0xF7}, c));  // page 13
    CHECK(!One({0xF0, 0x7D, 0x42, 0x01, 0x03, 0x00, 0x01, 0x00, 0x10, 0xF7}, c));  // page 0
    CHECK(!One({0xF0, 0x7D, 0x42, 0x01, 0x03, 0x01, 0x06, 0x00, 0x10, 0xF7}, c));  // slider 6
    CHECK(!One({0xF0, 0x7D, 0x42, 0x01, 0x03, 0x01, 0x01, 0x02, 0x00, 0xF7}, c));  // value 256
    CHECK(!One({0xF0, 0x7D, 0x42, 0x01, 0x04, 0x03, 0x01, 0xF7}, c));        // option bit 3
    CHECK(!One({0xF0, 0x7D, 0x42, 0x01, 0x03, 0x01, 0x01, 0x01, 0xF7}, c));  // short payload
    CHECK(!One({0xF0, 0x7D, 0x42, 0x01, 0x01, 0x00, 0xF7}, c));              // HELLO with payload
    CHECK(!One({0xF0, 0x7D, 0x42, 0x01, 0x02, 0x20, 0xF7}, c));              // program 32
    CHECK(!One({0xF0, 0x7D, 0x42, 0x01, 0x10, 0xF7}, c));                    // device-only command
    {
        Parser p;
        int n = Feed(p, {0xF0, 0x7D, 0x42, 0x01, 0x01}, c);
        for (size_t i = 0; i < kMaxFrame; i++) n += p.Feed(0x00, c) ? 1 : 0;
        n += p.Feed(0xF7, c) ? 1 : 0;
        CHECK(n == 0);  // oversize
        CHECK(Feed(p, {0xF0, 0x7D, 0x42, 0x01, 0x01, 0xF7}, c) == 1 && c.cmd == kHello);
    }

    // Realtime bytes inside a frame are ignored; garbage is skipped up to the next F0.
    CHECK(One({0xF0, 0x7D, 0xF8, 0x42, 0x01, 0xFE, 0x06, 0x00, 0xF8, 0xF7}, c) && c.cmd == kBypass &&
          c.b == 0);
    {
        Parser p;
        CHECK(Feed(p, {0x12, 0x34, 0xF7, 0xF0, 0x7D, 0x42, 0x90, 0x40, 0x01, 0xF7, 0x55}, c) == 0);
        CHECK(Feed(p, {0xF0, 0x7D, 0x42, 0x01, 0x02, 0x05, 0xF7}, c) == 1 && c.cmd == kProgram &&
              c.a == 5);
    }

    // Device -> host messages.
    {
        TxBuffer t;
        CHECK(t.AppendState({0, 22, 0x81, 1, false, 0xC0, 8192, 0x35, 0x04, 0x05}));
        CHECK(SameSysEx(t, {0xF0, 0x7D, 0x42, 0x01, 0x10, 0x00, 0x16, 0x01, 0x01, 0x01, 0x00, 0x01,
                            0x40, 0x40, 0x00, 0x00, 0x35, 0x04, 0x05, 0xF7}));
    }
    {
        TxBuffer t;
        const uint8_t v[6] = {0x84, 0x5F, 0x20, 0xB6, 0x55, 0x00};
        CHECK(t.AppendPage(1, v));
        CHECK(SameSysEx(t, {0xF0, 0x7D, 0x42, 0x01, 0x11, 0x01, 0x01, 0x04, 0x00, 0x5F, 0x00, 0x20,
                            0x01, 0x36, 0x00, 0x55, 0x00, 0x00, 0xF7}));
    }
    {
        TxBuffer t;
        CHECK(t.AppendLog("hi"));
        CHECK(SameSysEx(t, {0xF0, 0x7D, 0x42, 0x01, 0x12, 0x68, 0x69, 0xF7}));
        // USB packing: a 2-byte end packet.
        CHECK(Same(t, {0x04, 0xF0, 0x7D, 0x42, 0x04, 0x01, 0x12, 0x68, 0x06, 0x69, 0xF7, 0x00}));
    }
    {  // USB packing: 3- and 1-byte end packets.
        TxBuffer t;
        CHECK(t.AppendLog(""));
        CHECK(Same(t, {0x04, 0xF0, 0x7D, 0x42, 0x07, 0x01, 0x12, 0xF7}));
        t.Clear();
        CHECK(t.AppendLog("h"));
        CHECK(Same(t, {0x04, 0xF0, 0x7D, 0x42, 0x04, 0x01, 0x12, 0x68, 0x05, 0xF7, 0x00, 0x00}));
    }
    {  // Non-printable bytes become '?'; text is cut at kMaxLog.
        TxBuffer t;
        CHECK(t.AppendLog("a\tb\x80"));
        CHECK(SameSysEx(t, {0xF0, 0x7D, 0x42, 0x01, 0x12, 'a', '?', 'b', '?', 0xF7}));
        char longText[200];
        memset(longText, 'x', sizeof longText - 1);
        longText[sizeof longText - 1] = 0;
        t.Clear();
        CHECK(t.AppendLog(longText));
        CHECK(Unpack(t).size() == 6 + kMaxLog);
    }
    {  // A message that would overflow is refused whole.
        TxBuffer t;
        char text[kMaxLog + 1];
        memset(text, 'x', kMaxLog);
        text[kMaxLog] = 0;
        int n = 0;
        while (t.AppendLog(text)) n++;
        const size_t full = t.size();
        CHECK(n == 6 && full == 6 * 168);  // 126 bytes -> 42 packets of 4
        CHECK(!t.AppendLog(text) && t.size() == full);
        CHECK(t.AppendLog("") && t.size() == full + 8);
    }

    // ROM upload commands.
    {
        const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        CHECK(Crc32(digits, sizeof digits) == 0xCBF43926u);
    }
    CHECK(One({0xF0, 0x7D, 0x42, 0x01, 0x07, 0x0A, 0xF7}, c) && c.cmd == kRomBegin && c.a == 10);
    CHECK(One({0xF0, 0x7D, 0x42, 0x01, 0x0A, 0x03, 0xF7}, c) && c.cmd == kRomErase && c.a == 3);
    CHECK(!One({0xF0, 0x7D, 0x42, 0x01, 0x07, 0x0B, 0xF7}, c));  // slot 11
    CHECK(One({0xF0, 0x7D, 0x42, 0x01, 0x09, 0x03, 0x09, 0x59, 0x15, 0x33, 0x2F, 0xF7}, c) &&
          c.cmd == kRomCommit && c.a == 3 && c.crc == 0x9B2559AFu);
    CHECK(!One({0xF0, 0x7D, 0x42, 0x01, 0x09, 0x03, 0x10, 0x59, 0x15, 0x33, 0x2F, 0xF7}, c));  // crc > 32 bits

    // Mono-output override: the menu's masks only.
    CHECK(One({0xF0, 0x7D, 0x42, 0x01, 0x0B, 0x05, 0xF7}, c) && c.cmd == kOutMix && c.a == 5);
    CHECK(One({0xF0, 0x7D, 0x42, 0x01, 0x0B, 0x00, 0xF7}, c) && c.cmd == kOutMix && c.a == 0);
    CHECK(!One({0xF0, 0x7D, 0x42, 0x01, 0x0B, 0x02, 0xF7}, c));        // B alone: not a choice
    CHECK(!One({0xF0, 0x7D, 0x42, 0x01, 0x0B, 0x10, 0xF7}, c));        // beyond D
    CHECK(!One({0xF0, 0x7D, 0x42, 0x01, 0x0B, 0xF7}, c));              // no payload
    CHECK(!One({0xF0, 0x7D, 0x42, 0x01, 0x0B, 0x01, 0x00, 0xF7}, c));  // long payload
    {
        const auto f = [](size_t i) { return uint8_t(i * 3); };
        const std::vector<uint8_t> frame = RomData(0, 64, f);
        CHECK(frame.size() == 2 + kMaxFrame);
        const std::vector<uint8_t> head = {0xF0, 0x7D, 0x42, 0x01, 0x08, 0x00, 0x00,
                                           0x40, 0x00, 0x00, 0x00, 0x03, 0x00, 0x06};
        CHECK(std::vector<uint8_t>(frame.begin(), frame.begin() + 14) == head);
        Parser p;
        CHECK(Feed(p, frame, c) == 1 && c.cmd == kRomData && c.a == 0 && c.value == 64);
        bool same = true;
        for (size_t i = 0; i < kRomChunk; i++) same = same && p.Chunk()[i] == f(i);
        CHECK(same);
        CHECK(Feed(p, RomData(0, 2048 - 64, f), c) == 1 && c.value == 2048 - 64);  // last SBC chunk
        CHECK(Feed(p, RomData(3, 4096 - 64, f), c) == 1 && c.value == 4096 - 64);  // last NVS chunk
        CHECK(Feed(p, RomData(11, 0, f), c) == 0);       // slot 11
        CHECK(Feed(p, RomData(0, 32, f), c) == 0);       // not a chunk boundary
        CHECK(Feed(p, RomData(0, 2048, f), c) == 0);     // past a 2716
        CHECK(Feed(p, RomData(0, 0, f, true), c) == 0);  // high byte 2
        std::vector<uint8_t> shortFrame = frame;
        shortFrame.erase(shortFrame.end() - 2);  // 130-byte payload
        CHECK(Feed(p, shortFrame, c) == 0);
    }
    {
        TxBuffer t;
        CHECK(t.AppendRomAck(10, 4096));
        CHECK(SameSysEx(t, {0xF0, 0x7D, 0x42, 0x01, 0x14, 0x0A, 0x20, 0x00, 0xF7}));
        t.Clear();
        CHECK(t.AppendRomResult(0, kRomErrSocket));
        CHECK(SameSysEx(t, {0xF0, 0x7D, 0x42, 0x01, 0x15, 0x00, 0x04, 0xF7}));
        t.Clear();
        uint8_t state[kRomSlots] = {kRomBuiltIn, kRomBuiltIn, kRomBuiltIn};
        uint32_t crc[kRomSlots] = {0x1EAB567Eu, 0x5349B387u, 0x81F927F5u};
        CHECK(t.AppendRoms(true, state, crc));
        std::vector<uint8_t> want = {0xF0, 0x7D, 0x42, 0x01, 0x13, 0x01, 0x02, 0x01, 0x75, 0x2D,
                                     0x2C, 0x7E, 0x02, 0x05, 0x1A, 0x26, 0x67, 0x07, 0x02, 0x08,
                                     0x0F, 0x64, 0x4F, 0x75};
        want.insert(want.end(), 48, 0x00);
        want.push_back(0xF7);
        CHECK(Unpack(t) == want && want.size() == 6 + 67);
    }
    {  // RomUpload: a 2048-byte image with byte sum 1 (SBC1's socket).
        static uint8_t img[2048];
        for (size_t i = 0; i < sizeof img; i++) img[i] = uint8_t(i * 7 + 1);
        img[0] = uint8_t(img[0] + 1 - RomByteSum(img, sizeof img));
        const uint32_t crc = Crc32(img, sizeof img);
        const size_t chunks = sizeof img / kRomChunk;
        RomUpload u;
        u.Begin(0);
        CHECK(Upload(u, 0, img, chunks));
        CHECK(u.Commit(0, crc) == kRomOk && memcmp(u.Image(), img, sizeof img) == 0);
        CHECK(u.Commit(0, crc) == kRomErrSequence);  // the commit ended the upload

        u.Begin(0);  // a resent chunk is acked with the same next offset
        uint16_t next = 0;
        CHECK(u.Data(0, 0, img, next) && next == 64);
        CHECK(u.Data(0, 64, img + 64, next) && next == 128);
        CHECK(u.Data(0, 0, img, next) && next == 128);

        u.Begin(0);  // a gap cancels the upload
        CHECK(u.Data(0, 0, img, next));
        CHECK(!u.Data(0, 128, img + 128, next));
        CHECK(!u.Data(0, 64, img + 64, next));
        CHECK(u.Commit(0, crc) == kRomErrSequence);

        u.Begin(0);  // another slot's chunk cancels it too
        CHECK(!u.Data(1, 0, img, next));
        CHECK(u.Commit(0, crc) == kRomErrSequence);

        u.Begin(0);
        CHECK(Upload(u, 0, img, chunks - 1));
        CHECK(u.Commit(0, crc) == kRomErrIncomplete);
        CHECK(u.Commit(1, crc) == kRomErrSequence);  // no upload in progress

        u.Begin(0);
        CHECK(Upload(u, 0, img, chunks));
        CHECK(u.Commit(0, crc ^ 1) == kRomErrCrc);

        img[0] = uint8_t(img[0] + 1);  // byte sum 2: SBC2's
        u.Begin(0);
        CHECK(Upload(u, 0, img, chunks));
        CHECK(u.Commit(0, Crc32(img, sizeof img)) == kRomErrSocket);
        u.Begin(1);
        CHECK(Upload(u, 1, img, chunks));
        CHECK(u.Commit(1, Crc32(img, sizeof img)) == kRomOk);
    }

    {  // SPSC queue: capacity N, FIFO order.
        SpscQueue<uint8_t, 4> q;
        bool pushed = true;
        for (uint8_t i = 1; i <= 4; i++) pushed = q.Push(i) && pushed;
        CHECK(pushed);
        CHECK(!q.Push(5));
        uint8_t v = 0;
        bool order = true;
        for (uint8_t i = 1; i <= 4; i++) order = q.Pop(v) && v == i && order;
        CHECK(order);
        CHECK(!q.Pop(v));
        CHECK(q.Push(6) && q.Pop(v) && v == 6);
    }

    return CheckSummary("editor_protocol");
}
