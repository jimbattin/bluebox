import { expect, test } from "bun:test";
import { checkRomFile, eraseRom, flashRom, romVersion, type RomLink } from "./roms";
import { ROM_CHUNK, parseDeviceMessage, romSize, romSocket, type DeviceMessage } from "./protocol";

/** Zero-filled image whose byte sum is `sum`. */
const image = (n: number, sum: number) => {
  const d = new Uint8Array(n);
  d[0] = sum;
  return d;
};

const frame = (cmd: number, payload: number[]) =>
  parseDeviceMessage(Uint8Array.from([0xf0, 0x7d, 0x42, 0x01, cmd, ...payload, 0xf7]));

/** Fake pedal applying the device rules; can swallow the first ack of one chunk. */
function fakeLink(opts: { dropFirstAckAt?: number; commitCode?: number } = {}) {
  const listeners = new Set<(m: DeviceMessage) => void>();
  const sent: Uint8Array[] = [];
  const offsets: number[] = [];
  let received = 0;
  let dropped = false;
  const reply = (m: DeviceMessage | null) => {
    if (m) queueMicrotask(() => [...listeners].forEach((f) => f(m)));
  };
  const link: RomLink = {
    onMessage(f) {
      listeners.add(f);
      return () => listeners.delete(f);
    },
    send(b) {
      const u = Uint8Array.from(b);
      sent.push(u);
      const [, , , , cmd, slot = 0, hi = 0, lo = 0] = u;
      if (cmd === 0x07) {
        received = 0;
        reply(frame(0x14, [slot, 0, 0]));
      } else if (cmd === 0x08) {
        const off = (hi << 7) | lo;
        offsets.push(off);
        if (off === received) received += ROM_CHUNK;
        if (off > received) return; // gap: the device rejects
        if (off === opts.dropFirstAckAt && !dropped) {
          dropped = true;
          return;
        }
        reply(frame(0x14, [slot, (received >> 7) & 0x7f, received & 0x7f]));
      } else if (cmd === 0x09) {
        reply(frame(0x15, [slot, opts.commitCode ?? (received === romSize(slot) ? 0 : 2)]));
      } else if (cmd === 0x0a) reply(frame(0x15, [slot, 0]));
    },
  };
  return { link, offsets };
}

test("checkRomFile", () => {
  expect(checkRomFile(0, new Uint8Array(10))).toBe("SBC1 takes a 2048-byte 2716 image; this file is 10 bytes");
  expect(checkRomFile(3, new Uint8Array(2048))).toBe("NVS1 takes a 4096-byte 2732 image; this file is 2048 bytes");
  expect(checkRomFile(0, image(2048, 1))).toBeNull();
  expect(checkRomFile(0, image(2048, 2))).toBe("This is the SBC2 image (byte sum 2), not SBC1");
  expect(checkRomFile(3, image(4096, 2))).toBe("This is the NVS2 image (byte sum 2), not NVS1");
  expect(checkRomFile(0, image(2048, 9))).toBe("Byte sum 9 does not match SBC1 (expects 1)");
  expect(romVersion(0x1eab567e)).toBe("V8.2");
  expect(romVersion(1)).toBeNull();
});

test("flashRom resends a chunk whose ack was lost", async () => {
  const { link, offsets } = fakeLink({ dropFirstAckAt: 3 * ROM_CHUNK });
  const img = image(2048, romSocket(0));
  let last = 0;
  await flashRom(
    link,
    0,
    img,
    (f) => {
      last = f;
    },
    { ackMs: 50, resultMs: 500 },
  );
  expect(last).toBe(1);
  expect(offsets.filter((o) => o === 3 * ROM_CHUNK).length).toBe(2);
  expect(offsets.length).toBe(2048 / ROM_CHUNK + 1);
});

test("flashRom reports the COMMIT result", async () => {
  const { link } = fakeLink({ commitCode: 4 });
  await expect(flashRom(link, 3, image(4096, 1), () => {}, { ackMs: 50, resultMs: 500 })).rejects.toThrow(
    "byte sum does not match the socket",
  );
});

test("flashRom gives up without an answer", async () => {
  const link: RomLink = { send() {}, onMessage: () => () => {} };
  await expect(flashRom(link, 0, image(2048, 1), () => {}, { ackMs: 10, resultMs: 10 })).rejects.toThrow(
    "no answer from the pedal",
  );
});

test("eraseRom", async () => {
  await eraseRom(fakeLink().link, 4);
});
