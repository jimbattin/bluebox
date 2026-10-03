import { useEffect, useMemo, useReducer, useRef, useState } from "preact/hooks";
import { BarcLink, type LinkSnapshot } from "./midi";
import {
  MIX_MAX,
  OPT_DECAY_OPT,
  OPT_DYN_DECAY,
  OPT_MODE_ENH,
  selectProgram,
  setBypass,
  setMix,
  setOption,
  setOutMix,
  setSlider,
  type DeviceMessage,
  type OptionBit,
} from "./protocol";
import { PROGRAMS, type Page } from "./programs";
import { Header } from "./components/Header";
import { LedDisplay } from "./components/LedDisplay";
import { Fader } from "./components/Fader";
import { PageKeys } from "./components/PageKeys";
import { SwitchKeys, type SwitchKey } from "./components/SwitchKeys";
import { ProgramKeypad } from "./components/ProgramKeypad";
import { LogPanel } from "./components/LogPanel";
import { RomPanel } from "./components/RomPanel";
import { MonoOutKeys, MonoOutPanel } from "./components/MonoOut";

const HOLD_MS = 400;
const LOG_MAX = 200;

type StateMsg = Extract<DeviceMessage, { kind: "state" }>;

// The device is the source of truth: `values` and `mix` always hold its latest report.
// `local` holds what this editor last sent per control ("page:slider" or "mix"); it is
// shown instead while the control is held (dragged, or within HOLD_MS of its last input).
interface DevState {
  state: StateMsg | null;
  values: number[][];
  mix: number;
  local: Map<string, number>;
  log: string[];
  roms: Extract<DeviceMessage, { kind: "roms" }> | null;
}
type Action = { type: "device"; msg: DeviceMessage } | { type: "local"; key: string; value: number };

const initial = (): DevState => ({
  state: null,
  values: Array.from({ length: 12 }, () => new Array(6).fill(0)),
  mix: 0,
  local: new Map(),
  log: [],
  roms: null,
});

function reducer(s: DevState, a: Action): DevState {
  if (a.type === "local") return { ...s, local: new Map(s.local).set(a.key, a.value) };
  const m = a.msg;
  if (m.kind === "state") return { ...s, state: m, mix: m.mix };
  if (m.kind === "page") {
    if (m.page < 1 || m.page > 12) return s;
    const values = s.values.slice();
    values[m.page - 1] = m.values.slice();
    return { ...s, values };
  }
  if (m.kind === "roms") return { ...s, roms: m };
  if (m.kind === "log") return { ...s, log: [...s.log, m.text].slice(-LOG_MAX) };
  return s; // romAck / romResult: consumed by the flasher's own listener
}

const hex2 = (n: number) => n.toString(16).toUpperCase().padStart(2, "0");
const pad3 = (n: number) => String(n).padStart(3, "0");

function genericPage(page: number): Page {
  return {
    page,
    title: `PAGE ${page}`,
    sliders: [1, 2, 3, 4, 5, 6].map((n) => ({ short: `P${page} S${n}`, name: `P${page} S${n}`, help: "" })),
  };
}

export function App() {
  const link = useMemo(() => new BarcLink(), []);
  const [snap, setSnap] = useState<LinkSnapshot>(() => link.snapshot());
  const [dev, dispatch] = useReducer(reducer, undefined, initial);
  const [selPage, setSelPage] = useState(0); // page number; 0 = the first available page
  const [help, setHelp] = useState<string | null>(null);
  const holds = useRef(new Map<string, number>());
  const [, rerender] = useReducer((n: number) => n + 1, 0);

  const isHeld = (key: string) => {
    const t = holds.current.get(key);
    return t !== undefined && (t === Infinity || Date.now() - t < HOLD_MS);
  };
  // Released: the device's value takes over once the hold-off has passed.
  const touch = (key: string, dragging: boolean) => {
    holds.current.set(key, dragging ? Infinity : Date.now());
    if (!dragging) setTimeout(rerender, HOLD_MS + 10);
  };
  const shown = (key: string, device: number) => (isHeld(key) ? (dev.local.get(key) ?? device) : device);

  useEffect(() => {
    const offChange = link.onChange(setSnap);
    const offMsg = link.onMessage((msg) => dispatch({ type: "device", msg }));
    void link.start();
    return () => {
      offChange();
      offMsg();
      link.stop();
    };
  }, [link]);

  const st = dev.state;
  const linked = snap.linked && st !== null;
  const disabled = !linked || st?.runState === 3;
  const index = st?.programIndex ?? 0;
  const known = st !== null && PROGRAMS[index]?.id === st.programId;
  const def = known ? PROGRAMS[index] : null;
  const progName = st ? (def ? def.name : `PROGRAM ${hex2(st.programId)}`) : "";

  // A known program shows its family's fixed page keys. A key is disabled when the program lacks the
  // page or the pedal does not report it (every page while the program loads). An unknown program
  // lists the pages it reports.
  const shownPages: (Page & { available: boolean })[] = useMemo(() => {
    if (!st) return [];
    if (def)
      return def.pageKeys.map((k) => {
        const own = def.pages.find((p) => p.page === k.page);
        const reported = ((st.pageMask >> (k.page - 1)) & 1) === 1;
        return own ? { ...own, available: reported } : { ...k, sliders: [], available: false };
      });
    const out = [];
    for (let p = 1; p <= 12; p++)
      if (p !== 6 && (st.pageMask >> (p - 1)) & 1) out.push({ ...genericPage(p), available: true });
    return out;
  }, [st?.pageMask, st?.programId, st?.programIndex]);

  useEffect(() => setSelPage(0), [st?.programIndex, st?.programId]);
  const page = shownPages.find((p) => p.available && p.page === selPage) ?? shownPages.find((p) => p.available) ?? null;

  let line1 = snap.status;
  if (linked && st) {
    if (st.runState === 0) line1 = "LOADING…";
    else if (st.runState === 2) line1 = "LOAD FAILED";
    else if (st.runState === 3) line1 = "NO ROMS - FLASH THEM IN THE ROM PANEL";
    else line1 = `BANK ${Math.floor(index / 6) + 1} PROG ${(index % 6) + 1}  ${progName}`;
  }
  const line2 = linked && page ? page.title.toUpperCase() : "";

  const optionKey = (id: string, label: string, bit: OptionBit, mask: number, help: string): SwitchKey => ({
    id,
    label,
    lit: ((st?.options ?? 0) & mask) !== 0,
    help,
    onPress: () => link.send(setOption(bit, ((st?.options ?? 0) & mask) === 0)),
  });
  const keys: SwitchKey[] = [
    {
      id: "bypass",
      label: "BYPASS",
      lit: !!st?.bypass,
      help: "Bypass — Pass the clean dry signal (same as footswitch 1).",
      onPress: () => link.send(setBypass(!st?.bypass)),
    },
    optionKey(
      "modeenh",
      "MODE ENH",
      6,
      OPT_MODE_ENH,
      "Mode enhance — Slow random modulation of delay taps; smooths ringing. Switching it briefly glitches and cuts the tail.",
    ),
    optionKey(
      "decayopt",
      "DECAY OPT",
      7,
      OPT_DECAY_OPT,
      "Decay option — Once the input stops, the tail ends about 2.6 dB quieter.",
    ),
    optionKey(
      "dyndecay",
      "DYN DECAY",
      0,
      OPT_DYN_DECAY,
      "Dynamic decay — Enables the Low/Mid stop decay parameters once the input stops.",
    ),
  ];

  const onHelp = (t: string | null) => setHelp(t);
  const mix = shown("mix", dev.mix);

  return (
    <div class="barc">
      <Header link={snap} onInput={(id) => link.selectInput(id)} onOutput={(id) => link.selectOutput(id)} />
      <LedDisplay line1={line1} line2={line2} help={help ?? ""} />
      <section class="console">
        <div class="mix-col">
          <Fader
            id="mix"
            label="MIX"
            readout={`${Math.round((mix / MIX_MAX) * 100)}%`}
            caption="Dry ↔ Wet"
            value={mix}
            max={MIX_MAX}
            disabled={disabled}
            help={{ name: "Mix", help: "Balance between the dry input and the reverb (same as the pedal's mix knob)." }}
            onInput={(v) => {
              dispatch({ type: "local", key: "mix", value: v });
              link.sendThrottled("mix", setMix(v));
            }}
            onTouch={touch}
            onHelp={onHelp}
          />
        </div>
        <div class="bank-faders">
          {[0, 1, 2, 3, 4, 5].map((i) => {
            const param = page ? page.sliders[i] : null;
            const empty = !page || !param;
            const key = `${page?.page ?? 0}:${i}`;
            const value = page ? shown(key, dev.values[page.page - 1]?.[i] ?? 0) : 0;
            return (
              <Fader
                key={i}
                id={key}
                label={empty ? "—" : param.short}
                readout={empty ? "---" : pad3(value)}
                caption={empty ? "—" : param.name}
                value={value}
                max={255}
                disabled={disabled || empty}
                help={empty || !param.help ? undefined : param}
                onInput={(v) => {
                  if (!page) return;
                  dispatch({ type: "local", key, value: v });
                  link.sendThrottled(key, setSlider(page.page, i, v));
                }}
                onTouch={touch}
                onHelp={onHelp}
              />
            );
          })}
        </div>
        <div class="side-keys">
          <PageKeys pages={shownPages} selected={page?.page ?? 0} disabled={disabled} onSelect={setSelPage} />
        </div>
      </section>
      <section class="key-strip">
        <div class="strip-group">
          <span class="silk strip-title">OPTIONS</span>
          <SwitchKeys keys={keys} disabled={disabled} onHelp={onHelp} />
        </div>
        <div class="strip-divider" />
        <MonoOutKeys
          outMix={st?.outMix ?? -1}
          outDefault={st?.outDefault ?? 0}
          programId={st?.programId ?? 0}
          disabled={disabled}
          onSelect={(m) => link.send(setOutMix(m))}
          onHelp={onHelp}
        />
      </section>
      <ProgramKeypad current={st ? index : -1} disabled={disabled} onSelect={(i) => link.send(selectProgram(i))} />
      <MonoOutPanel linked={linked} progName={progName} outMix={st?.outMix ?? 0} outDefault={st?.outDefault ?? 0} />
      <RomPanel link={link} roms={dev.roms} linked={snap.linked} />
      <LogPanel lines={dev.log} />
    </div>
  );
}
