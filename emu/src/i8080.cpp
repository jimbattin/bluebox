// Intel 8080 instruction set: decode, flags and T-state counts.
#include "i8080.h"

I8080::I8080(Bus& bus) : bus_(bus) {}

void I8080::reset() {
    a = b = c = d = e = h = l = 0;
    sp = pc = 0;
    fs = fz = fac = fp = fcy = false;
    ie = halted = false;
}

uint8_t I8080::fetch8() { return bus_.read(pc++); }

uint16_t I8080::fetch16() {
    uint8_t lo = fetch8();
    uint8_t hi = fetch8();
    return uint16_t(hi << 8 | lo);
}

uint16_t I8080::read16(uint16_t addr) {
    uint8_t lo = bus_.read(addr);
    uint8_t hi = bus_.read(uint16_t(addr + 1));
    return uint16_t(hi << 8 | lo);
}

void I8080::write16(uint16_t addr, uint16_t v) {
    bus_.write(addr, uint8_t(v));
    bus_.write(uint16_t(addr + 1), uint8_t(v >> 8));
}

void I8080::push(uint16_t v) {
    sp -= 2;
    write16(sp, v);
}

uint16_t I8080::pop() {
    uint16_t v = read16(sp);
    sp += 2;
    return v;
}

uint8_t I8080::getReg(int r) {
    switch (r) {
    case 0: return b;
    case 1: return c;
    case 2: return d;
    case 3: return e;
    case 4: return h;
    case 5: return l;
    case 6: return bus_.read(hl());
    default: return a;
    }
}

void I8080::setReg(int r, uint8_t v) {
    switch (r) {
    case 0: b = v; break;
    case 1: c = v; break;
    case 2: d = v; break;
    case 3: e = v; break;
    case 4: h = v; break;
    case 5: l = v; break;
    case 6: bus_.write(hl(), v); break;
    default: a = v; break;
    }
}

uint16_t I8080::getPair(int rp) const {
    switch (rp) {
    case 0: return bc();
    case 1: return de();
    case 2: return hl();
    default: return sp;
    }
}

void I8080::setPair(int rp, uint16_t v) {
    switch (rp) {
    case 0: setBC(v); break;
    case 1: setDE(v); break;
    case 2: setHL(v); break;
    default: sp = v; break;
    }
}

// S Z 0 AC 0 P 1 CY
uint8_t I8080::psw() const {
    return uint8_t(fs << 7 | fz << 6 | fac << 4 | fp << 2 | 0x02 | fcy);
}

void I8080::setPsw(uint8_t f) {
    fs = f & 0x80;
    fz = f & 0x40;
    fac = f & 0x10;
    fp = f & 0x04;
    fcy = f & 0x01;
}

void I8080::setSZP(uint8_t r) {
    fs = r & 0x80;
    fz = r == 0;
    fp = !__builtin_parity(r);
}

bool I8080::cond(int cc) const {
    switch (cc) {
    case 0: return !fz;
    case 1: return fz;
    case 2: return !fcy;
    case 3: return fcy;
    case 4: return !fp;
    case 5: return fp;
    case 6: return !fs;
    default: return fs;
    }
}

void I8080::alu(int op, uint8_t v) {
    switch (op) {
    case 0:    // ADD
    case 1: {  // ADC
        int cin = (op == 1 && fcy) ? 1 : 0;
        int r = a + v + cin;
        fac = ((a & 0xF) + (v & 0xF) + cin) > 0xF;
        fcy = r > 0xFF;
        a = uint8_t(r);
        setSZP(a);
        break;
    }
    case 2:    // SUB
    case 3:    // SBB
    case 7: {  // CMP
        int cin = (op == 3 && fcy) ? 1 : 0;
        int r = a - v - cin;
        // The 8080 subtracts by adding the complement, so AC is the carry out of bit 3
        // of that sum: set when the low nibble does not borrow (the Z80's H is the opposite).
        fac = ((a & 0xF) - (v & 0xF) - cin) >= 0;
        fcy = r < 0;
        if (op != 7) a = uint8_t(r);
        setSZP(uint8_t(r));
        break;
    }
    case 4:  // ANA
        fac = ((a | v) >> 3) & 1;  // 8080 quirk: AC = bit 3 of (a | v)
        a &= v;
        fcy = false;
        setSZP(a);
        break;
    case 5:  // XRA
        a ^= v;
        fcy = fac = false;
        setSZP(a);
        break;
    default:  // ORA
        a |= v;
        fcy = fac = false;
        setSZP(a);
        break;
    }
}

void I8080::daa() {
    uint8_t corr = 0;
    uint8_t lo = a & 0xF, hi = a >> 4;
    if (lo > 9 || fac) corr |= 0x06;
    // hi >= 9 && lo > 9: the low correction carries into a high nibble of 9.
    if (hi > 9 || fcy || (hi >= 9 && lo > 9)) {
        corr |= 0x60;
        fcy = true;
    }
    fac = (lo + (corr & 0xF)) > 0xF;
    a = uint8_t(a + corr);
    setSZP(a);
}

int I8080::step() {
    if (halted) return 4;  // HLT: nothing resumes it (no interrupts); time still passes
    uint8_t op = fetch8();

    if ((op & 0xC0) == 0x40) {  // MOV / HLT
        if (op == 0x76) {
            halted = true;
            return 7;
        }
        int dst = (op >> 3) & 7, src = op & 7;
        setReg(dst, getReg(src));
        return (dst == 6 || src == 6) ? 7 : 5;
    }
    if ((op & 0xC0) == 0x80) {  // ALU r
        int src = op & 7;
        alu((op >> 3) & 7, getReg(src));
        return src == 6 ? 7 : 4;
    }

    switch (op) {
    // NOP and undocumented NOPs
    case 0x00: case 0x08: case 0x10: case 0x18:
    case 0x20: case 0x28: case 0x30: case 0x38:
        return 4;

    case 0x01: case 0x11: case 0x21: case 0x31:  // LXI
        setPair((op >> 4) & 3, fetch16());
        return 10;

    case 0x02: bus_.write(bc(), a); return 7;  // STAX B
    case 0x12: bus_.write(de(), a); return 7;  // STAX D
    case 0x0A: a = bus_.read(bc()); return 7;  // LDAX B
    case 0x1A: a = bus_.read(de()); return 7;  // LDAX D

    case 0x22: write16(fetch16(), hl()); return 16;  // SHLD
    case 0x2A: setHL(read16(fetch16())); return 16;  // LHLD
    case 0x32: bus_.write(fetch16(), a); return 13;  // STA
    case 0x3A: a = bus_.read(fetch16()); return 13;  // LDA

    case 0x03: case 0x13: case 0x23: case 0x33: {  // INX
        int rp = (op >> 4) & 3;
        setPair(rp, uint16_t(getPair(rp) + 1));
        return 5;
    }
    case 0x0B: case 0x1B: case 0x2B: case 0x3B: {  // DCX
        int rp = (op >> 4) & 3;
        setPair(rp, uint16_t(getPair(rp) - 1));
        return 5;
    }
    case 0x09: case 0x19: case 0x29: case 0x39: {  // DAD
        uint32_t r = uint32_t(hl()) + getPair((op >> 4) & 3);
        fcy = r > 0xFFFF;
        setHL(uint16_t(r));
        return 10;
    }

    case 0x04: case 0x0C: case 0x14: case 0x1C:
    case 0x24: case 0x2C: case 0x34: case 0x3C: {  // INR
        int r = (op >> 3) & 7;
        uint8_t v = uint8_t(getReg(r) + 1);
        fac = (v & 0xF) == 0;
        setSZP(v);
        setReg(r, v);
        return r == 6 ? 10 : 5;
    }
    case 0x05: case 0x0D: case 0x15: case 0x1D:
    case 0x25: case 0x2D: case 0x35: case 0x3D: {  // DCR
        int r = (op >> 3) & 7;
        uint8_t v = uint8_t(getReg(r) - 1);
        fac = (v & 0xF) != 0xF;  // as for SUB: set unless the low nibble borrowed
        setSZP(v);
        setReg(r, v);
        return r == 6 ? 10 : 5;
    }
    case 0x06: case 0x0E: case 0x16: case 0x1E:
    case 0x26: case 0x2E: case 0x36: case 0x3E: {  // MVI
        int r = (op >> 3) & 7;
        setReg(r, fetch8());
        return r == 6 ? 10 : 7;
    }

    case 0x07: {  // RLC
        fcy = a >> 7;
        a = uint8_t(a << 1 | fcy);
        return 4;
    }
    case 0x0F: {  // RRC
        fcy = a & 1;
        a = uint8_t(a >> 1 | fcy << 7);
        return 4;
    }
    case 0x17: {  // RAL
        bool out = a >> 7;
        a = uint8_t(a << 1 | fcy);
        fcy = out;
        return 4;
    }
    case 0x1F: {  // RAR
        bool out = a & 1;
        a = uint8_t(a >> 1 | fcy << 7);
        fcy = out;
        return 4;
    }
    case 0x27: daa(); return 4;
    case 0x2F: a = uint8_t(~a); return 4;  // CMA
    case 0x37: fcy = true; return 4;       // STC
    case 0x3F: fcy = !fcy; return 4;       // CMC

    case 0xC0: case 0xC8: case 0xD0: case 0xD8:
    case 0xE0: case 0xE8: case 0xF0: case 0xF8:  // Rcc
        if (cond((op >> 3) & 7)) {
            pc = pop();
            return 11;
        }
        return 5;

    case 0xC1: case 0xD1: case 0xE1: case 0xF1: {  // POP
        int rp = (op >> 4) & 3;
        uint16_t v = pop();
        if (rp == 3) {
            a = uint8_t(v >> 8);
            setPsw(uint8_t(v));
        } else {
            setPair(rp, v);
        }
        return 10;
    }
    case 0xC5: case 0xD5: case 0xE5: case 0xF5: {  // PUSH
        int rp = (op >> 4) & 3;
        push(rp == 3 ? uint16_t(a << 8 | psw()) : getPair(rp));
        return 11;
    }

    case 0xC2: case 0xCA: case 0xD2: case 0xDA:
    case 0xE2: case 0xEA: case 0xF2: case 0xFA: {  // Jcc
        uint16_t addr = fetch16();
        if (cond((op >> 3) & 7)) pc = addr;
        return 10;
    }
    case 0xC3: case 0xCB:  // JMP (CB undocumented)
        pc = fetch16();
        return 10;

    case 0xC4: case 0xCC: case 0xD4: case 0xDC:
    case 0xE4: case 0xEC: case 0xF4: case 0xFC: {  // Ccc
        uint16_t addr = fetch16();
        if (cond((op >> 3) & 7)) {
            push(pc);
            pc = addr;
            return 17;
        }
        return 11;
    }
    case 0xCD: case 0xDD: case 0xED: case 0xFD: {  // CALL (DD ED FD undocumented)
        uint16_t addr = fetch16();
        push(pc);
        pc = addr;
        return 17;
    }
    case 0xC9: case 0xD9:  // RET (D9 undocumented)
        pc = pop();
        return 10;

    case 0xC6: case 0xCE: case 0xD6: case 0xDE:
    case 0xE6: case 0xEE: case 0xF6: case 0xFE:  // ALU imm
        alu((op >> 3) & 7, fetch8());
        return 7;

    case 0xC7: case 0xCF: case 0xD7: case 0xDF:
    case 0xE7: case 0xEF: case 0xF7: case 0xFF:  // RST
        push(pc);
        pc = op & 0x38;
        return 11;

    case 0xD3: {  // OUT
        uint8_t port = fetch8();
        bus_.out(port, a);
        return 10;
    }
    case 0xDB: {  // IN
        uint8_t port = fetch8();
        a = bus_.in(port);
        return 10;
    }
    case 0xE3: {  // XTHL
        uint16_t v = read16(sp);
        write16(sp, hl());
        setHL(v);
        return 18;
    }
    case 0xE9: pc = hl(); return 5;  // PCHL
    case 0xF9: sp = hl(); return 5;  // SPHL
    case 0xEB: {                     // XCHG
        uint16_t t = de();
        setDE(hl());
        setHL(t);
        return 4;
    }
    case 0xF3: ie = false; return 4;  // DI
    case 0xFB: ie = true; return 4;   // EI
    }
    return 4;  // unreachable: every opcode is handled above
}
