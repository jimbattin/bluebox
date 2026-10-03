// Intel 8080 core. No interrupt input: the firmware's interrupts come from the 8251
// (LARC link, RST 7 -> 03EE), and the emulated machine has no LARC link (Machine
// injects LARC requests as calls instead).
#pragma once
#include <cstdint>

struct Bus {  // implemented by Machine
    virtual uint8_t read(uint16_t a) = 0;
    virtual void write(uint16_t a, uint8_t v) = 0;
    virtual uint8_t in(uint8_t port) = 0;
    virtual void out(uint8_t port, uint8_t v) = 0;

protected:
    ~Bus() = default;  // non-virtual: no deleting destructor, no operator delete
};

class I8080 {
public:
    explicit I8080(Bus& bus);
    int step();  // executes one instruction, returns T-states
    void reset();  // power-on register state (all zero, flags clear)

    uint8_t a = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
    uint16_t sp = 0, pc = 0;
    bool fs = false, fz = false, fac = false, fp = false, fcy = false;
    bool ie = false, halted = false;

private:
    Bus& bus_;

    uint8_t fetch8();
    uint16_t fetch16();
    uint16_t read16(uint16_t addr);
    void write16(uint16_t addr, uint16_t v);
    void push(uint16_t v);
    uint16_t pop();

    uint16_t bc() const { return uint16_t(b << 8 | c); }
    uint16_t de() const { return uint16_t(d << 8 | e); }
    uint16_t hl() const { return uint16_t(h << 8 | l); }
    void setBC(uint16_t v) { b = uint8_t(v >> 8); c = uint8_t(v); }
    void setDE(uint16_t v) { d = uint8_t(v >> 8); e = uint8_t(v); }
    void setHL(uint16_t v) { h = uint8_t(v >> 8); l = uint8_t(v); }

    uint8_t getReg(int r);  // 0..7 = B C D E H L M A
    void setReg(int r, uint8_t v);

    uint16_t getPair(int rp) const;  // 0..3 = BC DE HL SP
    void setPair(int rp, uint16_t v);

    uint8_t psw() const;
    void setPsw(uint8_t f);
    void setSZP(uint8_t r);
    bool cond(int cc) const;  // 0..7 = NZ Z NC C PO PE P M

    void alu(int op, uint8_t v);  // 0..7 = ADD ADC SUB SBB ANA XRA ORA CMP
    void daa();
};
