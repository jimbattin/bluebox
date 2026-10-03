// ROM flashing over the BARC link: slot table, image checks and the upload protocol
// (commands and rules in daisy/src/editor_protocol.h).

import {
  ROM_CHUNK,
  ROM_RESULT_TEXT,
  crc32,
  romBegin,
  romCommit,
  romData,
  romErase,
  romSize,
  romSocket,
  type DeviceMessage,
} from "./protocol";

export interface RomSlotInfo {
  slot: number;
  name: string;
  part: "2716" | "2732";
  board: "SBC" | "NVS";
}

/** Slot 0-2 = SBC1-3 (2716), 3-10 = NVS1-8 (2732). */
export const ROM_SLOT_INFO: RomSlotInfo[] = [
  ...[1, 2, 3].map((n, i): RomSlotInfo => ({ slot: i, name: `SBC${n}`, part: "2716", board: "SBC" })),
  ...[1, 2, 3, 4, 5, 6, 7, 8].map((n, i): RomSlotInfo => ({
    slot: 3 + i,
    name: `NVS${n}`,
    part: "2732",
    board: "NVS",
  })),
];

export const romSlotInfo = (slot: number): RomSlotInfo => {
  const info = ROM_SLOT_INFO[slot];
  if (!info) throw new RangeError(`no ROM slot ${slot}`);
  return info;
};

/** CRC-32 of each image of ROM set 224XL V8.2 (roms/224XL_v8_21/) -> version. */
const KNOWN_ROMS: Record<number, string> = {
  0x1eab567e: "V8.2", // SBC1
  0x5349b387: "V8.2", // SBC2
  0x81f927f5: "V8.2", // SBC3
  0x9b2559af: "V8.2", // NVS1
  0x6d849dec: "V8.2", // NVS2
  0x8893fe2d: "V8.2", // NVS3
  0x3d6f7053: "V8.2", // NVS4
  0x10c7aad5: "V8.2", // NVS5
  0xb352daf7: "V8.2", // NVS6
  0x851a14e3: "V8.2", // NVS7
  0xff8b8ac0: "V8.2", // NVS8
};

export const romVersion = (crc: number): string | null => KNOWN_ROMS[crc >>> 0] ?? null;

const byteSum = (d: Uint8Array): number => {
  let s = 0;
  for (const b of d) s = (s + b) & 0xff;
  return s;
};

/** Null if `data` can go into `slot`, else why not. */
export function checkRomFile(slot: number, data: Uint8Array): string | null {
  const info = romSlotInfo(slot);
  const size = romSize(slot);
  if (data.length !== size)
    return `${info.name} takes a ${size}-byte ${info.part} image; this file is ${data.length} bytes`;
  const sum = byteSum(data);
  if (sum === romSocket(slot)) return null;
  const other = ROM_SLOT_INFO.find((o) => o.slot !== slot && romSize(o.slot) === size && romSocket(o.slot) === sum);
  if (other) return `This is the ${other.name} image (byte sum ${sum}), not ${info.name}`;
  return `Byte sum ${sum} does not match ${info.name} (expects ${romSocket(slot)})`;
}

export interface RomLink {
  send(bytes: number[]): void;
  onMessage(f: (m: DeviceMessage) => void): () => void;
}

export interface RomTiming {
  ackMs: number;
  resultMs: number;
}
const DEFAULT_TIMING: RomTiming = { ackMs: 1000, resultMs: 5000 };
const ATTEMPTS = 3;

/**
 * Sends `bytes` and waits for a message accepted by `accept`. A romResult for `slot` with a non-zero code
 * at any point throws its text. On timeout the frame is resent until `attempts` are used.
 */
async function exchange(
  link: RomLink,
  slot: number,
  bytes: number[],
  accept: (m: DeviceMessage) => boolean,
  ms: number,
  attempts: number,
): Promise<void> {
  for (let attempt = 0; attempt < attempts; attempt++) {
    const answered = await new Promise<boolean>((resolve, reject) => {
      const done = (ok: boolean) => {
        clearTimeout(timer);
        off();
        resolve(ok);
      };
      const timer = setTimeout(() => done(false), ms);
      const off = link.onMessage((m) => {
        if (m.kind === "romResult" && m.slot === slot && m.code !== 0) {
          clearTimeout(timer);
          off();
          reject(new Error(ROM_RESULT_TEXT[m.code] ?? `error ${m.code}`));
        } else if (accept(m)) done(true);
      });
      link.send(bytes);
    });
    if (answered) return;
  }
  throw new Error("no answer from the pedal");
}

export async function flashRom(
  link: RomLink,
  slot: number,
  data: Uint8Array,
  onProgress: (fraction: number) => void,
  timing: RomTiming = DEFAULT_TIMING,
): Promise<void> {
  const ack = (next: number) => (m: DeviceMessage) => m.kind === "romAck" && m.slot === slot && m.next === next;
  onProgress(0);
  await exchange(link, slot, romBegin(slot), ack(0), timing.ackMs, ATTEMPTS);
  for (let off = 0; off < data.length; off += ROM_CHUNK) {
    await exchange(
      link,
      slot,
      romData(slot, off, data.subarray(off, off + ROM_CHUNK)),
      ack(off + ROM_CHUNK),
      timing.ackMs,
      ATTEMPTS,
    );
    onProgress((off + ROM_CHUNK) / data.length);
  }
  // No resend: a second COMMIT would arrive after the upload ended and be a sequence error.
  await exchange(
    link,
    slot,
    romCommit(slot, crc32(data)),
    (m) => m.kind === "romResult" && m.slot === slot && m.code === 0,
    timing.resultMs,
    1,
  );
}

export async function eraseRom(link: RomLink, slot: number, timing: RomTiming = DEFAULT_TIMING): Promise<void> {
  await exchange(
    link,
    slot,
    romErase(slot),
    (m) => m.kind === "romResult" && m.slot === slot && m.code === 0,
    timing.resultMs,
    1,
  );
}
