// Bluebox BARC SysEx protocol: F0 7D 42 01 <cmd> <payload> F7
// (shared with the pedal firmware, daisy/src/editor_protocol.h).

const MANUFACTURER = 0x7d;
const DEVICE = 0x42;
const VERSION = 0x01;
export const MIX_MAX = 16383;

const Cmd = {
  Hello: 0x01,
  Program: 0x02,
  Slider: 0x03,
  Option: 0x04,
  Mix: 0x05,
  Bypass: 0x06,
  State: 0x10,
  Page: 0x11,
  Log: 0x12,
  RomBegin: 0x07,
  RomData: 0x08,
  RomCommit: 0x09,
  RomErase: 0x0a,
  OutMix: 0x0b,
  Roms: 0x13,
  RomAck: 0x14,
  RomResult: 0x15,
} as const;

export type OptionBit = 0 | 6 | 7;
export const OPT_DYN_DECAY = 0x01;
export const OPT_MODE_ENH = 0x40;
export const OPT_DECAY_OPT = 0x80;

export type RunState = 0 | 1 | 2 | 3; // loading, running, failed, no ROMs
export type RomState = 0 | 1 | 2; // empty, flash, built-in
/** The mono-output menu: 0 = the program's default, else an outMix mask (A=1, B=2, C=4, D=8). */
export type OutMixChoice = 0 | 1 | 4 | 5 | 15;

const ROM_SLOTS = 11; // 0-2 SBC1-3 (2716), 3-10 NVS1-8 (2732)
export const ROM_CHUNK = 64;
export const romSize = (slot: number): number => (slot < 3 ? 2048 : 4096);
/** The byte sum the ROM in this socket must have (the firmware's own checksum test). */
export const romSocket = (slot: number): number => (slot < 3 ? slot + 1 : slot - 2);

export const ROM_RESULT_TEXT: Record<number, string> = {
  1: "transfer out of sequence",
  2: "transfer incomplete",
  3: "CRC mismatch in transfer",
  4: "byte sum does not match the socket",
  5: "QSPI flash write failed",
  6: "flash read-back mismatch",
};

/** IEEE CRC-32 (reflected, poly 0xEDB88320, init and xorout 0xFFFFFFFF). */
export function crc32(data: Uint8Array): number {
  let c = 0xffffffff;
  for (const b of data) {
    c ^= b;
    for (let i = 0; i < 8; i++) c = c & 1 ? (c >>> 1) ^ 0xedb88320 : c >>> 1;
  }
  return (c ^ 0xffffffff) >>> 0;
}

/** Byte i of a payload whose length the parser has already checked. */
const at = (p: Uint8Array, i: number): number => {
  const b = p[i];
  if (b === undefined) throw new RangeError(`payload byte ${i} of ${p.length}`);
  return b;
};

const value8 = (v: number): number[] => [(v >> 7) & 1, v & 0x7f];
const value14 = (v: number): number[] => [(v >> 7) & 0x7f, v & 0x7f];
const dec8 = (p: Uint8Array, i: number): number => ((at(p, i) & 1) << 7) | (at(p, i + 1) & 0x7f);
const dec14 = (p: Uint8Array, i: number): number => ((at(p, i) & 0x7f) << 7) | (at(p, i + 1) & 0x7f);
const value32 = (v: number): number[] => [
  (v >>> 28) & 0x0f,
  (v >>> 21) & 0x7f,
  (v >>> 14) & 0x7f,
  (v >>> 7) & 0x7f,
  v & 0x7f,
];
const dec32 = (p: Uint8Array, i: number): number =>
  (((at(p, i) & 0x0f) << 28) |
    ((at(p, i + 1) & 0x7f) << 21) |
    ((at(p, i + 2) & 0x7f) << 14) |
    ((at(p, i + 3) & 0x7f) << 7) |
    (at(p, i + 4) & 0x7f)) >>>
  0;

const frame = (cmd: number, payload: number[] = []): number[] => [
  0xf0,
  MANUFACTURER,
  DEVICE,
  VERSION,
  cmd,
  ...payload,
  0xf7,
];

export const hello = (): number[] => frame(Cmd.Hello);
export const selectProgram = (index: number): number[] => frame(Cmd.Program, [index & 0x7f]);
export const setSlider = (page: number, slider: number, v: number): number[] =>
  frame(Cmd.Slider, [page, slider, ...value8(Math.max(0, Math.min(255, Math.round(v))))]);
export const setOption = (bit: OptionBit, on: boolean): number[] => frame(Cmd.Option, [bit, on ? 1 : 0]);
export const setMix = (v14: number): number[] =>
  frame(Cmd.Mix, value14(Math.max(0, Math.min(MIX_MAX, Math.round(v14)))));
export const setBypass = (on: boolean): number[] => frame(Cmd.Bypass, [on ? 1 : 0]);
export const romBegin = (slot: number): number[] => frame(Cmd.RomBegin, [slot]);
export const romData = (slot: number, offset: number, chunk: Uint8Array): number[] =>
  frame(Cmd.RomData, [slot, ...value14(offset), ...Array.from(chunk, (b) => value8(b)).flat()]);
export const romCommit = (slot: number, crc: number): number[] => frame(Cmd.RomCommit, [slot, ...value32(crc)]);
export const romErase = (slot: number): number[] => frame(Cmd.RomErase, [slot]);
export const setOutMix = (mix: OutMixChoice): number[] => frame(Cmd.OutMix, [mix]);

export type DeviceMessage =
  | {
      kind: "state";
      programIndex: number;
      programCount: number;
      programId: number;
      runState: RunState;
      bypass: boolean;
      options: number;
      mix: number;
      pageMask: number;
      outMix: number;
      outDefault: number;
    }
  | { kind: "page"; page: number; values: number[] }
  | { kind: "log"; text: string }
  | { kind: "roms"; embedded: boolean; slots: { state: RomState; crc: number }[] }
  | { kind: "romAck"; slot: number; next: number }
  | { kind: "romResult"; slot: number; code: number };

/** Parses a complete SysEx message (F0 … F7). Returns null on a wrong header, version or length. */
export function parseDeviceMessage(data: Uint8Array): DeviceMessage | null {
  const n = data.length;
  if (n < 6 || data[0] !== 0xf0 || data[n - 1] !== 0xf7) return null;
  if (data[1] !== MANUFACTURER || data[2] !== DEVICE || data[3] !== VERSION) return null;
  const cmd = data[4];
  const p = data.subarray(5, n - 1);
  switch (cmd) {
    case Cmd.State:
      if (p.length !== 14) return null;
      return {
        kind: "state",
        programIndex: at(p, 0),
        programCount: at(p, 1),
        programId: dec8(p, 2),
        runState: at(p, 4) as RunState,
        bypass: at(p, 5) !== 0,
        options: dec8(p, 6),
        mix: dec14(p, 8),
        pageMask: dec14(p, 10),
        outMix: at(p, 12),
        outDefault: at(p, 13),
      };
    case Cmd.Page: {
      if (p.length !== 13) return null;
      const values: number[] = [];
      for (let i = 0; i < 6; i++) values.push(dec8(p, 1 + 2 * i));
      return { kind: "page", page: at(p, 0), values };
    }
    case Cmd.Roms: {
      if (p.length !== 1 + ROM_SLOTS * 6) return null;
      const slots: { state: RomState; crc: number }[] = [];
      for (let i = 0; i < ROM_SLOTS; i++) {
        const o = 1 + i * 6;
        slots.push({ state: at(p, o) as RomState, crc: dec32(p, o + 1) });
      }
      return { kind: "roms", embedded: at(p, 0) !== 0, slots };
    }
    case Cmd.RomAck:
      if (p.length !== 3) return null;
      return { kind: "romAck", slot: at(p, 0), next: dec14(p, 1) };
    case Cmd.RomResult:
      if (p.length !== 2) return null;
      return { kind: "romResult", slot: at(p, 0), code: at(p, 1) };
    case Cmd.Log:
      return { kind: "log", text: String.fromCharCode(...p) };
    default:
      return null;
  }
}
