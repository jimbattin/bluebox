// Host tests for the pedal control logic (src/pedal_controls.h). Fs2Gesture is
// driven through a model of libdaisy's Switch: an 8-bit shift register clocked
// once per block, Pressed() == 0xff and FallingEdge() == 0x80
// (libdaisy/src/hid/switch.h). A press latches on the 8th down block; a release
// edge fires on the 7th up block.
#include <stdint.h>

#include "check.h"
#include "pedal_controls.h"

struct SwitchModel {
    uint8_t s = 0;
    void Clock(bool down) { s = (uint8_t)((s << 1) | (down ? 1 : 0)); }
    bool Pressed() const { return s == 0xff; }
    bool Falling() const { return s == 0x80; }
};

struct Rig {
    SwitchModel fs2;
    Fs2Gesture g;
    int advances = 0;

    bool Step(bool down) {
        fs2.Clock(down);
        const bool a = g.Update(fs2.Pressed(), fs2.Falling());
        advances += a;
        return a;
    }
    void Run(bool down, int n) {
        for (int i = 0; i < n; i++) Step(down);
    }
};

int main() {
    CHECK(bankFromLevers(false, false) == 0);
    CHECK(bankFromLevers(true, false) == 1);
    CHECK(bankFromLevers(false, true) == 2);
    CHECK(bankFromLevers(true, true) == 3);

    CHECK(bankSize(0, 22) == 6);
    CHECK(bankSize(1, 22) == 6);
    CHECK(bankSize(2, 22) == 6);
    CHECK(bankSize(3, 22) == 4);

    CHECK(nextInBank(0, 22) == 1);
    CHECK(nextInBank(5, 22) == 0);
    CHECK(nextInBank(11, 22) == 6);
    CHECK(nextInBank(17, 22) == 12);
    CHECK(nextInBank(18, 22) == 19);
    CHECK(nextInBank(21, 22) == 18);

    {  // FS2 tap: exactly one advance, on the release edge.
        Rig r;
        r.Run(true, 50);
        CHECK(r.advances == 0);
        int edgeBlock = -1;
        for (int i = 0; i < 20; i++)
            if (r.Step(false)) edgeBlock = i;
        CHECK(r.advances == 1);
        CHECK(edgeBlock == 6);
    }
    {  // FS2 hold with a secondary edit: no advance; the next plain tap advances.
        Rig r;
        r.Run(true, 50);
        r.g.MarkEdited();
        r.Run(true, 50);
        r.Run(false, 20);
        CHECK(r.advances == 0);
        r.Run(true, 50);
        r.Run(false, 20);
        CHECK(r.advances == 1);
    }
    {  // Held() from the 8th down block until the release edge.
        Rig r;
        r.Run(true, 7);
        CHECK(!r.g.Held());
        r.Step(true);
        CHECK(r.g.Held());
        r.Run(true, 100);
        r.Run(false, 6);
        CHECK(r.g.Held());
        CHECK(r.Step(false));
        CHECK(!r.g.Held());
    }

    {  // Parked takeover.
        KnobTakeover k;
        CHECK(!k.Update(0, 0.9f));
        float pos[kKnobs] = {0.50f, 0.50f, 0.50f, 0.50f, 0.50f, 0.50f};
        k.Park(pos);
        CHECK(!k.Update(0, 0.515f));
        CHECK(k.Update(0, 0.53f));
        CHECK(k.Update(0, 0.50f));
        CHECK(!k.Update(1, 0.50f));
        k.Park(pos);
        CHECK(!k.Update(0, 0.50f));
    }
    {  // ParkOne re-parks one live knob; the others stay live.
        KnobTakeover k;
        float pos[kKnobs] = {0.50f, 0.50f, 0.50f, 0.50f, 0.50f, 0.50f};
        k.Park(pos);
        CHECK(k.Update(0, 0.60f));
        CHECK(k.Update(1, 0.60f));
        k.ParkOne(0, 0.60f);
        CHECK(!k.Update(0, 0.61f));
        CHECK(k.Update(1, 0.60f));
        CHECK(k.Update(0, 0.63f));
    }

    {  // Bank levers settle after kBankSettleBlocks consecutive blocks.
        BankSelector b;
        b.Init(0);
        int s = 0;
        for (int i = 0; i < 299; i++) s = b.Update(1);
        CHECK(s == 0);
        CHECK(b.Update(1) == 1);
    }
    {  // A settle is reported once, as an edge; Init clears a pending one.
        BankSelector b;
        b.Init(0);
        for (int i = 0; i < 299; i++) b.Update(1);
        CHECK(!b.TakeChange());
        b.Update(1);
        CHECK(b.TakeChange());
        CHECK(!b.TakeChange());
        b.Update(1);
        CHECK(!b.TakeChange());
        for (int i = 0; i < 300; i++) b.Update(2);
        b.Init(2);
        CHECK(!b.TakeChange());
    }
    {  // Passing through a lever combination does not settle on it.
        BankSelector b;
        b.Init(0);
        bool sawOne = false;
        for (int i = 0; i < 100; i++) sawOne |= b.Update(1) == 1;
        int s = 0;
        for (int i = 0; i < 299; i++) {
            s = b.Update(3);
            sawOne |= s == 1;
        }
        CHECK(!sawOne);
        CHECK(s == 0);
        CHECK(b.Update(3) == 3);
    }

    {  // Options: each load takes the preset's bits; lever flips and editor sets win until the next load.
        OptionState o;
        o.Init(0x40);                      // MODE ENH lever up at power-up
        o.Loaded(0x80);                    // preset DECAY OPT only
        CHECK(o.Update(0x40) == 0x80);     // lever position ignored until it moves
        CHECK(o.Update(0x00) == 0x80);     // MODE ENH lever flips down: already off
        CHECK(o.Update(0x40) == 0xC0);     // and up: on
        o.Set(0x01, true);
        CHECK(o.Update(0x40) == 0xC1);
        o.BeginLoad();
        o.Loaded(0x00);                    // next preset: earlier lever and editor touches dropped
        CHECK(o.Update(0x40) == 0x00);
        o.BeginLoad();
        CHECK(o.Update(0xC0) == 0x80);     // DECAY OPT lever flips up while loading
        o.Set(0x01, true);
        o.Loaded(0x40);                    // touched bits kept, MODE ENH from the preset
        CHECK(o.Desired() == 0xC1);
        CHECK(o.Update(0x40) == 0x41);     // DECAY OPT lever down overrides
    }

    CHECK(knobFor(0, 1, 1) == 2);
    CHECK(knobFor(0, 2, 1) == 2);
    CHECK(knobFor(1, 3, 4) == 1);
    CHECK(knobFor(0, 0, 0) == 0);
    CHECK(knobFor(1, 0, 0) == -1);
    CHECK(knobFor(0, 4, 0) == -1);

    CHECK(flashLit(1, 0));
    CHECK(flashLit(1, 149));
    CHECK(!flashLit(1, 150));
    CHECK(!flashLit(1, 1799));
    CHECK(flashLit(1, 1800));
    CHECK(flashLit(3, 600));
    CHECK(!flashLit(3, 750));
    CHECK(!flashLit(3, 900));
    CHECK(flashLit(3, 2400));
    CHECK(!flashLit(3, 2399));

    CHECK(sliderChanged(-1, 7));
    CHECK(!sliderChanged(100, 101));
    CHECK(sliderChanged(100, 102));
    CHECK(sliderChanged(1, 0));
    CHECK(sliderChanged(254, 255));
    CHECK(!sliderChanged(5, 5));

    CHECK(sliderPosition(0.0f) == 0);
    CHECK(sliderPosition(1.0f) == 255);
    CHECK(sliderPosition(0.5f) == 128);

    {  // Equal-power mix: exact ends, constant power, monotonic.
        float d, w;
        mixGains(0.0f, d, w);
        CHECK(d == 1.0f && w == 0.0f);
        mixGains(1.0f, d, w);
        CHECK(d == 0.0f && w == 1.0f);
        mixGains(-0.01f, d, w);
        CHECK(d == 1.0f && w == 0.0f);
        bool power = true, mono = true;
        float lastD = 1.0f, lastW = 0.0f;
        for (int i = 1; i < 100; i++) {
            mixGains(i / 100.0f, d, w);
            power &= std::fabs(d * d + w * w - 1.0f) < 1e-5f;
            mono &= d < lastD && w > lastW;
            lastD = d;
            lastW = w;
        }
        CHECK(power);
        CHECK(mono);
    }

    {  // Mono-output override record: erased sector, round trip, corruption, bad entry.
        uint8_t rec[OutMixTable::kRecordSize];
        OutMixTable t;
        t.mix[7] = 0x0F;
        std::memset(rec, 0xFF, sizeof rec);
        CHECK(!t.Load(rec));
        bool zero = true;
        for (uint8_t m : t.mix) zero &= m == 0;
        CHECK(zero);

        OutMixTable a, b;
        a.mix[0x0C] = 0x04;
        a.mix[0x21] = 0x05;
        a.Save(rec);
        CHECK(b.Load(rec));
        CHECK(std::memcmp(a.mix, b.mix, sizeof a.mix) == 0);

        rec[4 + 0x0C] = 0x05;  // entry changed, CRC stale
        CHECK(!b.Load(rec));
        CHECK(b.mix[0x21] == 0);

        a.mix[0x0C] = 0x02;  // B alone: not a menu choice, saved with a valid CRC
        a.Save(rec);
        CHECK(b.Load(rec));
        CHECK(b.mix[0x0C] == 0 && b.mix[0x21] == 0x05);
    }

    return CheckSummary("pedal_controls");
}
