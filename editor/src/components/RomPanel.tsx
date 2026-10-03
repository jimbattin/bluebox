import { useEffect, useRef, useState } from "preact/hooks";
import { crc32, type DeviceMessage } from "../protocol";
import { ROM_SLOT_INFO, checkRomFile, eraseRom, flashRom, romSlotInfo, romVersion, type RomLink } from "../roms";

type RomsMsg = Extract<DeviceMessage, { kind: "roms" }>;
interface Busy {
  slot: number;
  progress: number | null; // null = erasing
}

const hex8 = (n: number) => n.toString(16).toUpperCase().padStart(8, "0");

interface RomSocketProps {
  slot: number;
  state: "empty" | "flash" | "builtin" | "unknown";
  crc: number;
  busy: Busy | null;
  disabled: boolean;
  embedded: boolean;
  onPick: (slot: number) => void;
  onDrop: (slot: number, file: File) => void;
  onRemove: (slot: number) => void;
}

function RomSocket({ slot, state, crc, busy, disabled, embedded, onPick, onDrop, onRemove }: RomSocketProps) {
  const info = romSlotInfo(slot);
  const mine = busy?.slot === slot;
  const version = romVersion(crc) ?? "UNKNOWN";
  const pct = Math.round((busy?.progress ?? 0) * 100);
  const tag = state === "flash" ? "FLASHED" : state === "builtin" ? "BUILT-IN" : "—";
  return (
    <div class="rom-slot">
      <button
        class={`rom-socket state-${state}${mine ? " busy" : ""}`}
        disabled={disabled}
        title={`${info.name} (${info.part}): click or drop a .bin image to flash it`}
        onClick={() => onPick(slot)}
        onDragOver={(e) => {
          if (!disabled) e.preventDefault();
        }}
        onDrop={(e) => {
          e.preventDefault();
          const file = e.dataTransfer?.files[0];
          if (file && !disabled) onDrop(slot, file);
        }}
      >
        <span class="rom-chip">
          {mine ? (
            <span class="rom-paper">
              <span class="rom-progress">
                <span style={{ width: `${pct}%` }} />
              </span>
              <span class="rom-ver">{busy?.progress == null ? "ERASING" : `FLASHING ${pct}%`}</span>
            </span>
          ) : state === "empty" || state === "unknown" ? (
            <span class="rom-empty">{state === "empty" ? "EMPTY" : ""}</span>
          ) : (
            <span class="rom-paper">
              <span class="rom-ver">{version}</span>
              <span class="rom-name">{info.name}</span>
              <span class="rom-crc">{hex8(crc)}</span>
            </span>
          )}
        </span>
      </button>
      <div class="rom-caption">
        <span class="rom-silk">{info.name}</span>
        <span class="rom-tag">{tag}</span>
        {state === "flash" && (
          <button class="rom-key" disabled={disabled} onClick={() => onRemove(slot)}>
            {embedded ? "REVERT" : "ERASE"}
          </button>
        )}
      </div>
    </div>
  );
}

interface RomPanelProps {
  link: RomLink;
  roms: RomsMsg | null;
  linked: boolean;
}

export function RomPanel({ link, roms, linked }: RomPanelProps) {
  const [open, setOpen] = useState(false);
  const [busy, setBusy] = useState<Busy | null>(null);
  const [status, setStatus] = useState("");
  const input = useRef<HTMLInputElement>(null);
  const target = useRef(0);

  // A missing ROM opens the panel (never closes it).
  useEffect(() => {
    if (roms?.slots.some((s) => s.state === 0)) setOpen(true);
  }, [roms]);

  const disabled = busy !== null || !linked || roms === null;

  const flash = async (slot: number, file: File) => {
    const name = romSlotInfo(slot).name;
    const data = new Uint8Array(await file.arrayBuffer());
    const problem = checkRomFile(slot, data);
    if (problem) {
      setStatus(problem);
      return;
    }
    const crc = crc32(data);
    const version = romVersion(crc);
    if (!version && !confirm(`${name}: not a known 224XL V8.2 image (CRC ${hex8(crc)}). Flash it anyway?`)) return;
    setBusy({ slot, progress: 0 });
    setStatus("");
    try {
      await flashRom(link, slot, data, (progress) => setBusy({ slot, progress }));
      setStatus(`${name} flashed (${version ?? hex8(crc)}); the pedal reloads its ROMs`);
    } catch (e) {
      setStatus(`${name}: ${e instanceof Error ? e.message : String(e)}`);
    } finally {
      setBusy(null);
    }
  };

  const remove = async (slot: number) => {
    const name = romSlotInfo(slot).name;
    const verb = roms?.embedded ? "REVERT" : "ERASE";
    if (!confirm(`${verb} ${name}?`)) return;
    setBusy({ slot, progress: null });
    setStatus("");
    try {
      await eraseRom(link, slot);
      setStatus(roms?.embedded ? `${name} reverted to the built-in image` : `${name} erased`);
    } catch (e) {
      setStatus(`${name}: ${e instanceof Error ? e.message : String(e)}`);
    } finally {
      setBusy(null);
    }
  };

  const pick = (slot: number) => {
    target.current = slot;
    input.current?.click();
  };

  const socket = (slot: number) => {
    const s = roms?.slots[slot];
    const state = !s ? "unknown" : (["empty", "flash", "builtin"] as const)[s.state];
    return (
      <RomSocket
        key={slot}
        slot={slot}
        state={state}
        crc={s?.crc ?? 0}
        busy={busy}
        disabled={disabled}
        embedded={roms?.embedded ?? false}
        onPick={pick}
        onDrop={(sl, f) => void flash(sl, f)}
        onRemove={(sl) => void remove(sl)}
      />
    );
  };

  return (
    <section class="roms">
      <button class="log-toggle" onClick={() => setOpen(!open)}>
        {open ? "▾" : "▸"} ROMS
      </button>
      {open && (
        <div class="rom-board">
          <div class="rom-group">
            <div class="rom-title">SBC 80/10 · 2716</div>
            <div class="rom-row">{ROM_SLOT_INFO.filter((i) => i.board === "SBC").map((i) => socket(i.slot))}</div>
          </div>
          <div class="rom-group">
            <div class="rom-title">NVS · 2732</div>
            <div class="rom-row">{ROM_SLOT_INFO.filter((i) => i.board === "NVS").map((i) => socket(i.slot))}</div>
          </div>
          {roms === null && (
            <div class="rom-status">Waiting for the pedal's ROM report (needs firmware with ROM support)</div>
          )}
          {status && <div class="rom-status">{status}</div>}
          <input
            ref={input}
            type="file"
            accept=".bin,.rom"
            hidden
            onChange={(e) => {
              const el = e.currentTarget;
              const file = el.files?.[0];
              el.value = "";
              if (file) void flash(target.current, file);
            }}
          />
        </div>
      )}
    </section>
  );
}
